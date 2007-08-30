#ifndef PACKETFILTERCFG_H_
#define PACKETFILTERCFG_H_

#include <cfg/Cfg.h>
#include "reconf/FilterModule.h"

#include <vector>

class PacketFilterCfg
	: public Cfg
{
public:
	friend class ConfigManager;
	
	virtual PacketFilterCfg* create(XMLElement* e);
	virtual ~PacketFilterCfg();

	virtual std::string getName() { return "filter"; }
	
	virtual FilterModule* getInstance();
	
	virtual void connectInstances(Cfg* other)
	{
		getInstance()->connectTo(other->getInstance());
	}
	
protected:
	PacketFilterCfg(XMLElement* e);

private:
	std::vector<Cfg*> subCfgs;
	FilterModule* filter;
};

class PacketCountFilterCfg
	: public Cfg
{
public:
	friend class PacketFilterCfg;
	
	virtual PacketFilterCfg* create(XMLElement* e) {return NULL; };
	
	virtual ~PacketCountFilterCfg();

	virtual std::string getName() { return "countBased"; }
	
	int getInterval() { return getInt("interval", 0); }
	int getSpacing()  { return getInt("spacing", 0); }

	virtual Module* getInstance();
	
protected:
	PacketCountFilterCfg(XMLElement *e);
};











class InfoElementId
	: public Cfg

{
public:
	InfoElementId(XMLElement* e)
		: Cfg(e)
	{

	}

	/*
		for (XMLNode::XMLNodeSet::iterator i = xmlNodePtr i = p->xmlChildrenNode;
		while (NULL != i) {
			if (m.tagMatches(i, "ieName")) {
				ieName = m.getContent(i);
			} else if (m.tagMatches(i, "match")) {
				match = m.getContent(i);
			} else if (m.tagMatches(i, "modifier")) {
				modifier = m.getContent(i);
			} else if (m.tagMatches(i, "ieId")) {
				ieId = m.getContent(i);
			} else if (m.tagMatches(i, "ieLength")) {
				ieLength = m.getContent(i);
			} else if (m.tagMatches(i, "enterpriseNumber")) {
				enterpriseNumber = m.getContent(i);
			}
			i = i->next;
		}
		std::transform(ieName.begin(), ieName.end(), ieName.begin(), std::tolower);
	}
	 */

	std::string getIeName() {
		std::string ret = get("ieName");
		std::transform(ret.begin(), ret.end(), ret.begin(), std::tolower);
		return  ret;
	}
	std::string getIeId() { return get("ieId"); }
	std::string getIeLength() { return get("ieLength"); }
	std::string getMatch() { return get("match"); }
	std::string getEnterpriseNumber() { return get("enterpriseNumber"); }
	std::string getModifier() { return get("modifier"); }
};

#endif /*PACKETFILTERCFG_H_*/