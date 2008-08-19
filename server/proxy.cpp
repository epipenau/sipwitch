// Copyright (C) 2006-2007 David Sugar, Tycho Softworks.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "server.h"

NAMESPACE_SIPWITCH
using namespace UCOMMON_NAMESPACE;

static bool rtp_running = false;
static volatile timeout_t interval = 50;
static volatile time_t refresh = 60;
static volatile time_t updated = 0;
static int priority = 0;
static const char *iface = NULL;

proxy proxy::rtp;

proxy::listener::listener() :
JoinableThread(priority)
{
}

void proxy::listener::run(void)
{
	time_t now;

	rtp_running = true;
	while(rtp_running) {
		rtpproxy::slice(interval);
		time(&now);
		if(now > updated) {
			updated = now + refresh;
			rtpproxy::publish((const char *)proxy::rtp.published);
		}
	}
	process::errlog(DEBUG1, "stopping rtpproxy thread");
	JoinableThread::exit();
}

proxy::proxy() :
service::callback(-1)
{
	port = 9000;
	count = 0; 	
	published = NULL;
	thr = NULL;
	nets = NULL;
}

void proxy::start(service *cfg) 
{
	assert(cfg != NULL);

	if(count) {
		rtpproxy::startup(count, port, stack::sip.family, iface);
		process::errlog(INFO, "rtp proxy started for %d ports", count);
		thr = new proxy::listener();
		thr->start();
	} 
}

void proxy::stop(service *cfg)
{
	assert(cfg != NULL);

	rtp_running = false;
	if(thr) {
		process::errlog(DEBUG1, "rtp proxy stopping");
		delete thr;
		rtpproxy::shutdown();
	}
}

void proxy::snapshot(FILE *fp) 
{ 
	assert(fp != NULL);

	fprintf(fp, "RTP PROXY:\n"); 
} 

bool proxy::publishingAddress(const char *address)
{
	rtpproxy::publish(address);
	return true;
}

bool proxy::reload(service *cfg)
{
	assert(cfg != NULL);	

	caddr_t mp;
	volatile char *vp;
	const char *key = NULL, *value;
	linked_pointer<service::keynode> sp = cfg->getList("proxy");
	int val;

	updated = 0l;

	while(is(sp)) {
		key = sp->getId();
		value = sp->getPointer();
		if(key && value) {
			if(!stricmp(key, "count") && !isConfigured())
				count = atoi(value);
			else if(!stricmp(key, "interface") && !isConfigured())
				iface = strdup(value);
			else if(!stricmp(key, "interval"))
				interval = atol(value);
			else if(!stricmp(key, "priority") && !isConfigured())
				priority = atoi(value);
			else if(!stricmp(key, "port") && !isConfigured())
				port = atoi(value);
			else if(!stricmp(key, "refresh"))
				refresh = atoi(value);
			else if(!stricmp(key, "publish") || !stricmp(key, "public")) {
				vp = published;
				published = strdup(value);
				if(vp)
					free((void *)vp);
			}			
		}
		sp.next();
	}

	sp = cfg->getList("rtpproxy.networks");
	nets = NULL;

	while(is(sp)) {
		key = sp->getId();
		value = sp->getPointer();
		if(key && value) {
			mp = (caddr_t)cfg->alloc_locked(sizeof(cidr));
			new(mp) cidr(&nets, value, key);
		}
		sp.next();
	}
	return true;
}

void proxy::copy(stack::session *target, stack::session *source)
{
	target->proxying = source->proxying;
	String::set(target->network, sizeof(target->network), source->network);
	memcpy(&target->iface, &source->iface, sizeof(target->iface));
}

bool proxy::assign(stack::call *cr, unsigned count)
{
	if(cr->rtp)
		return true;

	cr->rtp = rtpproxy::create(count);
	if(cr->rtp)
		return true;

	return false;
}

bool proxy::isRequired(void)
{
	// we can still enable gateway mode, even in ipv6...
	if(server::flags_gateway)
		return true;

#ifdef	AF_INET6
	// we otherwise do not require just proxy for generic "external" access if 
	// we are ipv6...
	if(stack::sip.family == AF_INET6)
		return false;
#endif

	if(proxy::rtp.count)
		return true;
	
	return false;
}

bool proxy::classify(stack::session *sid, struct sockaddr *addr)
{
	service::keynode *cfg;
	stack::session *src = sid->parent->source;

	sid->proxying = rtpproxy::NO_PROXY;

	if(server::flags_gateway) {
		if(!addr)
			addr = rtpproxy::getPublished();

		stack::getInterface((struct sockaddr *)(&sid->iface), addr);
		if(sid != src) {
			if(!Socket::equal((struct sockaddr *)(&sid->iface), (struct sockaddr *)(&src->iface))) {
				sid->proxying = rtpproxy::GATEWAY_PROXY;
				String::set(sid->network, sizeof(sid->network), "-");
				return true;
			}
		}
		return false;
	}

	if(!proxy::rtp.count)
		return false;

	cfg = server::getConfig();
	if(!cfg)
		return false;

	linked_pointer<cidr> np = proxy::rtp.nets;

	if(!is(np)) {
		server::release(cfg);
		return false;
	}

	cidr *member = NULL;
	unsigned top = 0;
		
	while(is(np) && addr) {
		if(np->isMember(addr)) {
			if(np->getMask() > top) {
				top = np->getMask();
				member = *np;
			}
		}
		np.next();	
	}

	if(member)
		String::set(sid->network, sizeof(sid->network), member->getName());
	else
		String::set(sid->network, sizeof(sid->network), "-");

	server::release(cfg);

	// we only do external-to-internal proxy for ipv4.  We can still need rtp
	// proxying if joining two different subnets (private & public), however.

	if(src != sid) {
		if(!stricmp(src->network, "-") && stricmp(sid->network, "-")) {
			if(stack::sip.family == AF_INET)
				sid->proxying = rtpproxy::REMOTE_PROXY;
		}
		else if(stricmp(src->network, "-") && !stricmp(sid->network, "-")) {
			if(stack::sip.family == AF_INET)
				sid->proxying = rtpproxy::LOCAL_PROXY;
		}
		else if(!stricmp(src->network, "-") && !stricmp(sid->network, "-")) {
			if(stack::sip.family == AF_INET)
				sid->proxying = rtpproxy::BRIDGE_PROXY;
		}
		else if(stricmp(src->network, sid->network)) {
			stack::getInterface((struct sockaddr *)(&sid->iface), addr);
			sid->proxying = rtpproxy::SUBNET_PROXY;
		}
	}
	else if(addr)
		stack::getInterface((struct sockaddr *)(&sid->iface), addr);	

	if(sid->proxying != rtpproxy::NO_PROXY)
		return true;
	
	return false;		
}

END_NAMESPACE
