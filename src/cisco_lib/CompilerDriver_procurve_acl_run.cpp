/* 

                          Firewall Builder

                 Copyright (C) 2010 NetCitadel, LLC

  Author:  Vadim Kurland     vadim@vk.crocodile.org

  $Id$

  This program is free software which we release under the GNU General Public
  License. You may redistribute and/or modify this program under the terms
  of that license as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  To get a copy of the GNU General Public License, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "../../config.h"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <memory>

#include <assert.h>
#include <cstring>
#include <iomanip>

#include "CompilerDriver_procurve_acl.h"

#include "PolicyCompiler_procurve_acl.h"
#include "RoutingCompiler_procurve_acl.h"
#include "OSConfigurator_procurve.h"
#include "NamedObjectsAndGroupsSupport.h"

#include "fwbuilder/Resources.h"
#include "fwbuilder/FWObjectDatabase.h"
#include "fwbuilder/XMLTools.h"
#include "fwbuilder/FWException.h"
#include "fwbuilder/Firewall.h"
#include "fwbuilder/Interface.h"
#include "fwbuilder/Policy.h"
#include "fwbuilder/NAT.h"
#include "fwbuilder/Routing.h"

#include "fwcompiler/Preprocessor.h"

#include "fwbuilder/Resources.h"
#include "fwbuilder/FWObjectDatabase.h"
#include "fwbuilder/FWException.h"
#include "fwbuilder/Cluster.h"
#include "fwbuilder/ClusterGroup.h"
#include "fwbuilder/Firewall.h"
#include "fwbuilder/Interface.h"
#include "fwbuilder/Policy.h"
#include "fwbuilder/StateSyncClusterGroup.h"
#include "fwbuilder/FailoverClusterGroup.h"

#include <QStringList>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>



using namespace std;
using namespace libfwbuilder;
using namespace fwcompiler;


QString CompilerDriver_procurve_acl::assembleManifest(Cluster *cluster, Firewall* fw, bool cluster_member)
{
    QString script_buffer;
    QTextStream script(&script_buffer, QIODevice::WriteOnly);
    QString ofname = determineOutputFileName(cluster, fw, cluster_member, ".fw");
    script << ";" << MANIFEST_MARKER << "* " << this->escapeFileName(ofname) << endl;
    return script_buffer;
}

QString CompilerDriver_procurve_acl::assembleFwScript(Cluster *cluster,
                                                Firewall *fw,
                                                bool cluster_member,
                                                OSConfigurator *oscnf)
{
    Configlet script_skeleton(fw, "procurve", "script_skeleton");
    Configlet top_comment(fw, "procurve", "top_comment");

    script_skeleton.setVariable("system_configuration_script",
                                QString::fromUtf8(system_configuration_script.c_str()));
    script_skeleton.setVariable("policy_script", 
                                QString::fromUtf8(policy_script.c_str()));
    script_skeleton.setVariable("nat_script", 
                                QString::fromUtf8(nat_script.c_str()));
    script_skeleton.setVariable("routing_script", 
                                QString::fromUtf8(routing_script.c_str()));

    FWOptions* options = fw->getOptionsObject();
    options->setStr("prolog_script", options->getStr("procurve_acl_prolog_script"));
    options->setStr("epilog_script", options->getStr("procurve_acl_epilog_script"));

    assembleFwScriptInternal(cluster, fw, cluster_member, oscnf, &script_skeleton, &top_comment, ";");
    return script_skeleton.expand();
}

QString CompilerDriver_procurve_acl::run(const std::string &cluster_id,
                                         const std::string &firewall_id,
                                         const std::string &single_rule_id)
{
    Cluster *cluster = NULL;
    if (!cluster_id.empty())
        cluster = Cluster::cast(
            objdb->findInIndex(objdb->getIntId(cluster_id)));

    Firewall *fw = Firewall::cast(
        objdb->findInIndex(objdb->getIntId(firewall_id)));
    assert(fw);

    try
    {
        // Copy rules from the cluster object
        populateClusterElements(cluster, fw);

        commonChecks2(cluster, fw);

        // Note that fwobjectname may be different from the name of the
        // firewall fw This happens when we compile a member of a cluster
        current_firewall_name = fw->getName().c_str();

        QString ofname = determineOutputFileName(cluster, fw, !cluster_id.empty(), ".fw");

        FWOptions* options = fw->getOptionsObject();

        string fwvers = fw->getStr("version");
        if (fwvers == "") fw->setStr("version", "K.13");

        string platform = fw->getStr("platform");

        bool procurve_acl_acl_basic = options->getBool("procurve_acl_acl_basic");
        bool procurve_acl_acl_no_clear = options->getBool("procurve_acl_acl_no_clear");
        bool procurve_acl_acl_substitution = options->getBool("procurve_acl_acl_substitution");
        bool procurve_acl_add_clear_statements = options->getBool("procurve_acl_add_clear_statements");

        if ( !procurve_acl_acl_basic &&
             !procurve_acl_acl_no_clear &&
             !procurve_acl_acl_substitution )
        {
            if ( procurve_acl_add_clear_statements )
                options->setBool("procurve_acl_acl_basic",true);
            else
                options->setBool("procurve_acl_acl_no_clear",true);
        }

        std::auto_ptr<OSConfigurator_procurve> oscnf(new OSConfigurator_procurve(objdb, fw, false));

        oscnf->prolog();
        oscnf->processFirewallOptions();

        list<FWObject*> all_policies = fw->getByType(Policy::TYPENAME);

        vector<int> ipv4_6_runs;

        if (!single_rule_compile_on)
            system_configuration_script = safetyNetInstall(fw);

        NamedObjectManager named_object_manager(fw);

        // command line options -4 and -6 control address family for which
        // script will be generated. If "-4" is used, only ipv4 part will 
        // be generated. If "-6" is used, only ipv6 part will be generated.
        // If neither is used, both parts will be done.

        if (options->getStr("ipv4_6_order").empty() ||
            options->getStr("ipv4_6_order") == "ipv4_first")
        {
            if (ipv4_run) ipv4_6_runs.push_back(AF_INET);
            if (ipv6_run) ipv4_6_runs.push_back(AF_INET6);
        }

        if (options->getStr("ipv4_6_order") == "ipv6_first")
        {
            if (ipv6_run) ipv4_6_runs.push_back(AF_INET6);
            if (ipv4_run) ipv4_6_runs.push_back(AF_INET);
        }

        for (vector<int>::iterator i=ipv4_6_runs.begin(); 
             i!=ipv4_6_runs.end(); ++i)
        {
            int policy_af = *i;
            bool ipv6_policy = (policy_af == AF_INET6);

            // Count rules for each address family
            int policy_count = 0;

            for (list<FWObject*>::iterator p=all_policies.begin();
                 p!=all_policies.end(); ++p)
            {
                Policy *policy = Policy::cast(*p);
                if (policy->matchingAddressFamily(policy_af)) policy_count++;
            }
            if (policy_count)
            {
                std::auto_ptr<Preprocessor> prep(new Preprocessor(objdb, fw, false));
                if (inTestMode()) prep->setTestMode();
                if (inEmbeddedMode()) prep->setEmbeddedMode();
                prep->compile();
            }

            for (list<FWObject*>::iterator p=all_policies.begin();
                 p!=all_policies.end(); ++p )
            {
                Policy *policy = Policy::cast(*p);

                if (!policy->matchingAddressFamily(policy_af)) continue;

                PolicyCompiler_procurve_acl c(objdb, fw, ipv6_policy, oscnf.get());

                c.setNamedObjectManager(&named_object_manager);
                c.setSourceRuleSet( policy );
                c.setRuleSetName(policy->getName());

                c.setSingleRuleCompileMode(single_rule_id);
                if (inTestMode()) c.setTestMode();
                if (inEmbeddedMode()) c.setEmbeddedMode();
                c.setDebugLevel( dl );
                if (rule_debug_on) c.setDebugRule(  drp );
                c.setVerbose( verbose );

                if ( c.prolog() > 0 )
                {
                    c.compile();
                    c.epilog();

                    if (!single_rule_compile_on)
                    {
                        if (ipv6_policy)
                        {
                            policy_script += "\n\n";
                            policy_script += "; ================ IPv6\n";
                            policy_script += "\n\n";
                        } else
                        {
                            policy_script += "\n\n";
                            policy_script += "; ================ IPv4\n";
                            policy_script += "\n\n";
                        }
                    }

                    if (c.haveErrorsAndWarnings())
                    {
                        all_errors.push_back(c.getErrors("").c_str());
                    }
                    policy_script +=  c.getCompiledScript();

                } else
                    info(" Nothing to compile in Policy");
            }

            if (!ipv6_policy)
            {
                list<FWObject*> all_routing = fw->getByType(Routing::TYPENAME);
                RuleSet *routing = RuleSet::cast(all_routing.front());

                // currently routing is supported only for ipv4
                RoutingCompiler_procurve_acl r(objdb, fw, false, oscnf.get());

                r.setNamedObjectManager(&named_object_manager);
                r.setSourceRuleSet(routing);
                r.setRuleSetName(routing->getName());

                r.setSingleRuleCompileMode(single_rule_id);
                if (inTestMode()) r.setTestMode();
                if (inEmbeddedMode()) r.setEmbeddedMode();
                r.setDebugLevel( dl );
                if (rule_debug_on) r.setDebugRule(  drp );
                r.setVerbose( verbose );
                
                if ( r.prolog() > 0 )
                {
                    r.compile();
                    r.epilog();

                    if (r.haveErrorsAndWarnings())
                    {
                        all_errors.push_back(r.getErrors("").c_str());
                    }

                    routing_script += r.getCompiledScript();
                } else
                    info(" Nothing to compile in Routing");
            }
        }

        if (haveErrorsAndWarnings())
        {
            all_errors.push_front(getErrors("").c_str());
        }


        if (single_rule_compile_on)
        {
            return formSingleRuleCompileOutput(
                QString::fromUtf8((policy_script + routing_script).c_str()));
        }

        QString script_buffer = assembleFwScript(
            cluster, fw, !cluster_id.empty(), oscnf.get());

        ofname = getAbsOutputFileName(ofname);

        info("Output file name: " + ofname.toStdString());
        QFile fw_file(ofname);
        if (fw_file.open(QIODevice::WriteOnly))
        {
            QTextStream fw_str(&fw_file);
            fw_str << script_buffer;
            fw_file.close();
            fw_file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                   QFile::ReadGroup | QFile::ReadOther |
                                   QFile::ExeOwner | 
                                   QFile::ExeGroup |
                                   QFile::ExeOther );

            info(" Compiled successfully");
        } else
        {
            QString err(" Failed to open file %1 for writing: %2; Current dir: %3");
            abort(err.arg(fw_file.fileName())
                  .arg(fw_file.error()).arg(QDir::current().path()).toStdString());
        }
    }
    catch (FWException &ex)
    {
        return QString::fromUtf8(ex.toString().c_str());
    }

    return "";
}


