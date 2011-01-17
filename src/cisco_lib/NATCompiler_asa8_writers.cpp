/* 

                          Firewall Builder

                 Copyright (C) 2011 NetCitadel, LLC

  Author:  Vadim Kurland     vadim@fwbuilder.org

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

#include "config.h"

#include "NATCompiler_asa8.h"
#include "NamedObject.h"
#include "ASA8TwiceNatLogic.h"
#include "NamedObjectsAndGroupsSupport.h"

#include "fwbuilder/FWObjectDatabase.h"
#include "fwbuilder/RuleElement.h"
#include "fwbuilder/NAT.h"
#include "fwbuilder/AddressRange.h"
#include "fwbuilder/ICMPService.h"
#include "fwbuilder/TCPService.h"
#include "fwbuilder/UDPService.h"
#include "fwbuilder/Interface.h"
#include "fwbuilder/IPv4.h"
#include "fwbuilder/Network.h"
#include "fwbuilder/Resources.h"

#include <assert.h>
#include <sstream>

#include <QStringList>


using namespace libfwbuilder;
using namespace fwcompiler;
using namespace std;


bool NATCompiler_asa8::PrintClearCommands::processNext()
{
    string version = compiler->fw->getStr("version");
    string platform = compiler->fw->getStr("platform");

    slurp();
    if (tmp_queue.size()==0) return false;

    compiler->output << endl;

    if ( !compiler->fw->getOptionsObject()->getBool("pix_acl_no_clear") &&
         !compiler->inSingleRuleCompileMode())
    {
        compiler->output << Resources::platform_res[platform]->getResourceStr(
            string("/FWBuilderResources/Target/options/") +
            "version_" + version + "/pix_commands/clear_xlate") << endl;
        compiler->output << Resources::platform_res[platform]->getResourceStr(
            string("/FWBuilderResources/Target/options/") +
            "version_" + version + "/pix_commands/clear_nat") << endl;
        compiler->output << Resources::platform_res[platform]->getResourceStr(
            string("/FWBuilderResources/Target/options/") +
            "version_" + version+"/pix_commands/clear_object") << endl;
    }

    return true;
}


NATCompiler_asa8::PrintRule::PrintRule(const std::string &name) :
    NATCompiler_pix::PrintRule(name) 
{ }


void NATCompiler_asa8::PrintRule::printNONAT(libfwbuilder::NATRule *rule)
{
    printSDNAT(rule);
}

void NATCompiler_asa8::PrintRule::printSNAT(libfwbuilder::NATRule *rule)
{
    printSDNAT(rule);
}

void NATCompiler_asa8::PrintRule::printDNAT(libfwbuilder::NATRule *rule)
{
    printSDNAT(rule);
}

QString NATCompiler_asa8::PrintRule::printSingleObject(FWObject *obj)
{
    NATCompiler_asa8 *pix_comp = dynamic_cast<NATCompiler_asa8*>(compiler);

    if (Address::cast(obj) && Address::cast(obj)->isAny()) return "any";

    NamedObject* asa8_object = pix_comp->named_objects_manager->getNamedObject(obj);
    if (asa8_object) return asa8_object->getCommandWord();

    for (FWObject::iterator i=CreateObjectGroups::object_groups->begin();
         i!=CreateObjectGroups::object_groups->end(); ++i)
    {
        BaseObjectGroup *og = dynamic_cast<BaseObjectGroup*>(*i);
        assert(og!=NULL);
        if (og->getId() == obj->getId()) return obj->getName().c_str();
    }

    if (Interface::isA(obj) && obj->isChildOf(compiler->fw)) return "interface";

    QString err("Found unknown object '%1' in the NAT rule: it is not "
                "an ASA8 object, object group or an interface of the firewall");
    throw FWException(err.arg(obj->getName().c_str()).toStdString());
}

void NATCompiler_asa8::PrintRule::printSDNAT(NATRule *rule)
{
    //NATCompiler_asa8 *pix_comp = dynamic_cast<NATCompiler_asa8*>(compiler);

    FWOptions *ropt = rule->getOptionsObject();

    QStringList cmd;

    RuleElementOSrc *osrc_re = rule->getOSrc();
    assert(osrc_re!=NULL);
    FWObject *osrc = FWReference::getObject(osrc_re->front());

    RuleElementODst *odst_re = rule->getODst();
    assert(odst_re!=NULL);
    FWObject *odst = FWReference::getObject(odst_re->front());

    RuleElementOSrv *osrv_re = rule->getOSrv();
    assert(osrv_re!=NULL);
    FWObject *osrv = FWReference::getObject(osrv_re->front());

    RuleElementTSrc *tsrc_re = rule->getTSrc();
    assert(tsrc_re!=NULL);

    Address  *tdst = compiler->getFirstTDst(rule);  assert(tdst);
    Service  *tsrv = compiler->getFirstTSrv(rule);  assert(tsrv);

    Interface *o_iface = Interface::cast(compiler->dbcopy->findInIndex(
                                             rule->getInt("nat_iface_orig")));
    Interface *t_iface = Interface::cast(compiler->dbcopy->findInIndex(
                                             rule->getInt("nat_iface_trn")));

    cmd << QString("nat (%1,%2)")
        .arg(o_iface->getLabel().c_str())
        .arg(t_iface->getLabel().c_str());

    cmd << "source";

    switch (ASA8TwiceNatStaticLogic(rule).getType())
    {
    case ASA8TwiceNatStaticLogic::STATIC:
        cmd << "static";
        break;
    case ASA8TwiceNatStaticLogic::DYNAMIC:
        cmd << "dynamic";
        break;
    }

    cmd << printSingleObject(osrc);

    if (tsrc_re->isAny())
        cmd << printSingleObject(osrc);
    else
    {
        // TSrc can have one object, which can be either an address or
        // a group, or two objects in which case one must be an interface
        if (tsrc_re->size() == 1)
        {
            FWObject *tsrc = FWReference::getObject(tsrc_re->front());
            cmd << printSingleObject(tsrc);
        } else
        {
            // first, print name of the address or group, then interface
            bool have_interface = false;
            for (FWObject::iterator it=tsrc_re->begin(); it!=tsrc_re->end(); ++it)
            {
                FWObject *obj = FWReference::getObject(*it);
                if (Interface::isA(obj))
                {
                    have_interface = true;
                    continue;
                } else
                {
                    cmd << printSingleObject(obj);
                    break;
                }
            }
            if (have_interface) cmd << "interface";
        }
    }

    // only need "destination" part if ODst is not any
    if (!odst_re->isAny())
    {
        // ASA documentation says destination translation is always "static"
        cmd << "destination" << "static";
        cmd << printSingleObject(odst);

        if (tdst->isAny())
            cmd << printSingleObject(odst);
        else
            cmd << printSingleObject(tdst);
    }

    if (!osrv_re->isAny())
    {
        cmd << "service";
        cmd << printSingleObject(osrv);
        if (tsrv->isAny())
            cmd << printSingleObject(osrv);
        else
            cmd << printSingleObject(tsrv);
    }

    if (ropt->getBool("asa8_nat_dns")) cmd << "dns";

    cmd << QString("description \"%1\"").arg(rule->getLabel().c_str());

    compiler->output << cmd.join(" ").toStdString() << endl;
}




