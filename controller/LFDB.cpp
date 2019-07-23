/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2019  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

#include "LFDB.hpp"

#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>

#include "../osdep/OSUtils.hpp"
#include "../ext/cpp-httplib/httplib.h"

namespace ZeroTier
{

LFDB::LFDB(EmbeddedNetworkController *const nc,const Identity &myId,const char *path,const char *lfOwnerPrivate,const char *lfOwnerPublic,const char *lfNodeHost,int lfNodePort,bool storeOnlineState) :
	DB(nc,myId,path),
	_nc(nc),
	_myId(myId),
	_lfOwnerPrivate(lfOwnerPrivate),
	_lfOwnerPublic(lfOwnerPublic),
	_lfNodeHost(lfNodeHost),
	_lfNodePort(lfNodePort),
	_running(true),
	_ready(false),
	_storeOnlineState(storeOnlineState)
{
	_syncThread = std::thread([this]() {
		char controllerAddress[24];
		_myId.address().toString(controllerAddress);

		httplib::Client htcli(_lfNodeHost.c_str(),_lfNodePort,600);
		std::ostringstream query;
		int64_t timeRangeStart = 0;
		while (_running) {
			query.clear();
			query
				<< '{'
					<< "\"Ranges\":[{"
						<< "\"Name\": \"com.zerotier.controller.lfdb:" << controllerAddress << "/network\","
						<< "\"Range\": [ 0,18446744073709551615 ]"
					<< "}],"
					<< "\"TimeRange\": [ " << timeRangeStart << ",18446744073709551615 ],"
					<< "\"MaskingKey\":\"" << controllerAddress << "\","
					<< "\"Owners\":[\"" << _lfOwnerPublic << "\"]"
				<< '}';
			auto resp = htcli.Post("/query",query.str(),"application/json");
			if (resp->status == 200) {
				nlohmann::json results(OSUtils::jsonParse(resp->body));
				if ((results.is_array())&&(results.size() > 0)) {
					for(std::size_t ri=0;ri<results.size();++ri) {
						nlohmann::json &rset = results[ri];
						if ((rset.is_array())&&(rset.size() > 0)) {
							nlohmann::json &result = rset[0];
							if (result.is_object()) {
								nlohmann::json &record = result["Record"];
								if (record.is_object()) {
									int64_t ts = record["Timestamp"];
									std::string value = result["Value"];
									nlohmann::json network(OSUtils::jsonParse(value));
									if (network.is_object()) {
										std::string idstr = network["id"];
									}
								}
							}
						}
					}
				}
			} else {
				fprintf(stderr,"ERROR: LFDB: %d from node: %s" ZT_EOL_S,resp->status,resp->body.c_str());
			}

			query.clear();
			query
				<< '{'
					<< "\"Ranges\":[{"
						<< "\"Name\": \"com.zerotier.controller.lfdb:" << controllerAddress << "/network\","
						<< "\"Range\": [ 0,18446744073709551615 ]"
					<< "},{"
						<< "\"Name\": \"com.zerotier.controller.lfdb:" << controllerAddress << "/network/member\","
						<< "\"Range\": [ 0,18446744073709551615 ]"
					<< "}],"
					<< "\"TimeRange\": [ " << timeRangeStart << ",18446744073709551615 ],"
					<< "\"MaskingKey\":\"" << controllerAddress << "\","
					<< "\"Owners\":[\"" << _lfOwnerPublic << "\"]"
				<< '}';
			auto resp = htcli.Post("/query",query.str(),"application/json");
			if (resp->status == 200) {
				nlohmann::json results(OSUtils::jsonParse(resp->body));
				if ((results.is_array())&&(results.size() > 0)) {
					for(std::size_t ri=0;ri<results.size();++ri) {
						nlohmann::json &rset = results[ri];
						if ((rset.is_array())&&(rset.size() > 0)) {
							nlohmann::json &result = rset[0];
							if (result.is_object()) {
								nlohmann::json &record = result["Record"];
								if (record.is_object()) {
									int64_t ts = record["Timestamp"];
									std::string value = result["Value"];
									nlohmann::json member(OSUtils::jsonParse(value));
									if (member.is_object()) {
										std::string nwidstr = member["nwid"];
										std::string idstr = member["id"];
									}
								}
							}
						}
					}
				}
			} else {
				fprintf(stderr,"ERROR: LFDB: %d from node: %s" ZT_EOL_S,resp->status,resp->body.c_str());
			}

			timeRangeStart = time(nullptr) - 120; // start next query 2m before now to avoid losing updates
			_ready = true;

			// Delay 2s between queries, checking running flag every 100ms
			for(int k=0;k<20;++k) {
				if (!_running)
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	});
}

LFDB::~LFDB()
{
	_running = false;
	_syncThread.join();
}

bool LFDB::waitForReady()
{
	while (!_ready) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

bool LFDB::isReady()
{
	return (_ready);
}

void LFDB::save(nlohmann::json *orig,nlohmann::json &record)
{
	if (orig) {
		if (*orig != record) {
			record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1;
		}
	} else {
		record["revision"] = 1;
	}

	const std::string objtype = record["objtype"];
	if (objtype == "network") {
		const uint64_t nwid = OSUtils::jsonIntHex(record["id"],0ULL);
		if (nwid) {
			nlohmann::json old;
			get(nwid,old);
			if ((!old.is_object())||(old != record)) {
			}
		}
	} else if (objtype == "member") {
		const uint64_t nwid = OSUtils::jsonIntHex(record["nwid"],0ULL);
		const uint64_t id = OSUtils::jsonIntHex(record["id"],0ULL);
		if ((id)&&(nwid)) {
			nlohmann::json network,old;
			get(nwid,network,id,old);
			if ((!old.is_object())||(old != record)) {
			}
		}
	}
}

void LFDB::eraseNetwork(const uint64_t networkId)
{
	// TODO
}

void LFDB::eraseMember(const uint64_t networkId,const uint64_t memberId)
{
	// TODO
}

void LFDB::nodeIsOnline(const uint64_t networkId,const uint64_t memberId,const InetAddress &physicalAddress)
{
	std::lock_guard<std::mutex> l(_state_l);
	auto nw = _state.find(networkId);
	if (nw != _state.end()) {
		auto m = nw->second.members.find(memberId);
		if (m != nw->second.members.end()) {
			m->second.lastOnlineTime = OSUtils::now();
			if (physicalAddress)
				m->second.lastOnlineAddress = physicalAddress;
			m->second.lastOnlineDirty = true;
		}
	}
}

#if 0
FileDB::FileDB(EmbeddedNetworkController *const nc,const Identity &myId,const char *path) :
	DB(nc,myId,path),
	_networksPath(_path + ZT_PATH_SEPARATOR_S + "network"),
	_tracePath(_path + ZT_PATH_SEPARATOR_S + "trace"),
	_onlineChanged(false),
	_running(true)
{
	OSUtils::mkdir(_path.c_str());
	OSUtils::lockDownFile(_path.c_str(),true);
	OSUtils::mkdir(_networksPath.c_str());
	OSUtils::mkdir(_tracePath.c_str());

	std::vector<std::string> networks(OSUtils::listDirectory(_networksPath.c_str(),false));
	std::string buf;
	for(auto n=networks.begin();n!=networks.end();++n) {
		buf.clear();
		if ((n->length() == 21)&&(OSUtils::readFile((_networksPath + ZT_PATH_SEPARATOR_S + *n).c_str(),buf))) {
			try {
				nlohmann::json network(OSUtils::jsonParse(buf));
				const std::string nwids = network["id"];
				if (nwids.length() == 16) {
					nlohmann::json nullJson;
					_networkChanged(nullJson,network,false);
					std::string membersPath(_networksPath + ZT_PATH_SEPARATOR_S + nwids + ZT_PATH_SEPARATOR_S "member");
					std::vector<std::string> members(OSUtils::listDirectory(membersPath.c_str(),false));
					for(auto m=members.begin();m!=members.end();++m) {
						buf.clear();
						if ((m->length() == 15)&&(OSUtils::readFile((membersPath + ZT_PATH_SEPARATOR_S + *m).c_str(),buf))) {
							try {
								nlohmann::json member(OSUtils::jsonParse(buf));
								const std::string addrs = member["id"];
								if (addrs.length() == 10) {
									nlohmann::json nullJson2;
									_memberChanged(nullJson2,member,false);
								}
							} catch ( ... ) {}
						}
					}
				}
			} catch ( ... ) {}
		}
	}

	_onlineUpdateThread = std::thread([this]() {
		unsigned int cnt = 0;
		while (this->_running) {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			if ((++cnt % 20) == 0) { // 5 seconds
				std::lock_guard<std::mutex> l(this->_online_l);
				if (!this->_running) return;
				if (this->_onlineChanged) {
					char p[4096],atmp[64];
					for(auto nw=this->_online.begin();nw!=this->_online.end();++nw) {
						OSUtils::ztsnprintf(p,sizeof(p),"%s" ZT_PATH_SEPARATOR_S "%.16llx-online.json",_networksPath.c_str(),(unsigned long long)nw->first);
						FILE *f = fopen(p,"wb");
						if (f) {
							fprintf(f,"{");
							const char *memberPrefix = "";
							for(auto m=nw->second.begin();m!=nw->second.end();++m) {
								fprintf(f,"%s\"%.10llx\":{" ZT_EOL_S,memberPrefix,(unsigned long long)m->first);
								memberPrefix = ",";
								InetAddress lastAddr;
								const char *timestampPrefix = " ";
								int cnt = 0;
								for(auto ts=m->second.rbegin();ts!=m->second.rend();) {
									if (cnt < 25) {
										if (lastAddr != ts->second) {
											lastAddr = ts->second;
											fprintf(f,"%s\"%lld\":\"%s\"" ZT_EOL_S,timestampPrefix,(long long)ts->first,ts->second.toString(atmp));
											timestampPrefix = ",";
											++cnt;
											++ts;
										} else {
											ts = std::map<int64_t,InetAddress>::reverse_iterator(m->second.erase(std::next(ts).base()));
										}
									} else {
										ts = std::map<int64_t,InetAddress>::reverse_iterator(m->second.erase(std::next(ts).base()));
									}
								}
								fprintf(f,"}");
							}
							fprintf(f,"}" ZT_EOL_S);
							fclose(f);
						}
					}
					this->_onlineChanged = false;
				}
			}
		}
	});
}

FileDB::~FileDB()
{
	try {
		_online_l.lock();
		_running = false;
		_online_l.unlock();
		_onlineUpdateThread.join();
	} catch ( ... ) {}
}

bool FileDB::waitForReady() { return true; }
bool FileDB::isReady() { return true; }

void FileDB::save(nlohmann::json *orig,nlohmann::json &record)
{
	char p1[4096],p2[4096],pb[4096];
	try {
		if (orig) {
			if (*orig != record) {
				record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1;
			}
		} else {
			record["revision"] = 1;
		}

		const std::string objtype = record["objtype"];
		if (objtype == "network") {
			const uint64_t nwid = OSUtils::jsonIntHex(record["id"],0ULL);
			if (nwid) {
				nlohmann::json old;
				get(nwid,old);
				if ((!old.is_object())||(old != record)) {
					OSUtils::ztsnprintf(p1,sizeof(p1),"%s" ZT_PATH_SEPARATOR_S "%.16llx.json",_networksPath.c_str(),nwid);
					if (!OSUtils::writeFile(p1,OSUtils::jsonDump(record,-1)))
						fprintf(stderr,"WARNING: controller unable to write to path: %s" ZT_EOL_S,p1);
					_networkChanged(old,record,true);
				}
			}
		} else if (objtype == "member") {
			const uint64_t id = OSUtils::jsonIntHex(record["id"],0ULL);
			const uint64_t nwid = OSUtils::jsonIntHex(record["nwid"],0ULL);
			if ((id)&&(nwid)) {
				nlohmann::json network,old;
				get(nwid,network,id,old);
				if ((!old.is_object())||(old != record)) {
					OSUtils::ztsnprintf(pb,sizeof(pb),"%s" ZT_PATH_SEPARATOR_S "%.16llx" ZT_PATH_SEPARATOR_S "member",_networksPath.c_str(),(unsigned long long)nwid);
					OSUtils::ztsnprintf(p1,sizeof(p1),"%s" ZT_PATH_SEPARATOR_S "%.10llx.json",pb,(unsigned long long)id);
					if (!OSUtils::writeFile(p1,OSUtils::jsonDump(record,-1))) {
						OSUtils::ztsnprintf(p2,sizeof(p2),"%s" ZT_PATH_SEPARATOR_S "%.16llx",_networksPath.c_str(),(unsigned long long)nwid);
						OSUtils::mkdir(p2);
						OSUtils::mkdir(pb);
						if (!OSUtils::writeFile(p1,OSUtils::jsonDump(record,-1)))
							fprintf(stderr,"WARNING: controller unable to write to path: %s" ZT_EOL_S,p1);
					}
					_memberChanged(old,record,true);
				}
			}
		} else if (objtype == "trace") {
			const std::string id = record["id"];
			if (id.length() > 0) {
				OSUtils::ztsnprintf(p1,sizeof(p1),"%s" ZT_PATH_SEPARATOR_S "%s.json",_tracePath.c_str(),id.c_str());
				OSUtils::writeFile(p1,OSUtils::jsonDump(record,-1));
			}
		}
	} catch ( ... ) {} // drop invalid records missing fields
}

void FileDB::eraseNetwork(const uint64_t networkId)
{
	nlohmann::json network,nullJson;
	get(networkId,network);
	char p[16384];
	OSUtils::ztsnprintf(p,sizeof(p),"%s" ZT_PATH_SEPARATOR_S "%.16llx.json",_networksPath.c_str(),networkId);
	OSUtils::rm(p);
	OSUtils::ztsnprintf(p,sizeof(p),"%s" ZT_PATH_SEPARATOR_S "%.16llx-online.json",_networksPath.c_str(),networkId);
	OSUtils::rm(p);
	OSUtils::ztsnprintf(p,sizeof(p),"%s" ZT_PATH_SEPARATOR_S "%.16llx" ZT_PATH_SEPARATOR_S "member",_networksPath.c_str(),(unsigned long long)networkId);
	OSUtils::rmDashRf(p);
	_networkChanged(network,nullJson,true);
	std::lock_guard<std::mutex> l(this->_online_l);
	this->_online.erase(networkId);
	this->_onlineChanged = true;
}

void FileDB::eraseMember(const uint64_t networkId,const uint64_t memberId)
{
	nlohmann::json network,member,nullJson;
	get(networkId,network);
	get(memberId,member);
	char p[4096];
	OSUtils::ztsnprintf(p,sizeof(p),"%s" ZT_PATH_SEPARATOR_S "%.16llx" ZT_PATH_SEPARATOR_S "member" ZT_PATH_SEPARATOR_S "%.10llx.json",_networksPath.c_str(),networkId,memberId);
	OSUtils::rm(p);
	_memberChanged(member,nullJson,true);
	std::lock_guard<std::mutex> l(this->_online_l);
	this->_online[networkId].erase(memberId);
	this->_onlineChanged = true;
}

void FileDB::nodeIsOnline(const uint64_t networkId,const uint64_t memberId,const InetAddress &physicalAddress)
{
	char mid[32],atmp[64];
	OSUtils::ztsnprintf(mid,sizeof(mid),"%.10llx",(unsigned long long)memberId);
	physicalAddress.toString(atmp);
	std::lock_guard<std::mutex> l(this->_online_l);
	this->_online[networkId][memberId][OSUtils::now()] = physicalAddress;
	this->_onlineChanged = true;
}
#endif

} // namespace ZeroTier