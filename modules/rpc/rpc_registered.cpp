// Anope IRC Services <https://www.anope.org/>
//
// Copyright (C) 2003-2026 Anope Contributors
//
// Anope is free software. You can use, modify, and/or distribute it under the
// terms of version 2 of the GNU General Public License. See docs/LICENSE.txt
// for the complete terms of this license and docs/AUTHORS.txt for a list of
// contributors.
//
// Based on the original code of Epona by Lara
// Based on the original code of Services by Andy Church
//
// SPDX-License-Identifier: GPL-2.0-only

// Registered-channel and suspension data over RPC. rpc_data's
// anope.listChannels walks the live channel list, so a registered channel
// that is currently empty (and not persistent) never appears in it and no
// ChannelInfo field (founder, registration time, access list) is exposed at
// all; and NSSuspendInfo/CSSuspendInfo are ExtensibleItem<T> whose
// serializer is a no-op, so suspensions never show up in "extensions".
// This module fills both gaps:
//
//   anope.listRegisteredChannels [name|full]
//   anope.registeredChannel <channel>
//   anope.listSuspendedAccounts

#include "module.h"
#include "modules/rpc.h"
#include "modules/suspend.h"

enum
{
	// Used by anope.registeredChannel.
	ERR_NO_SUCH_TARGET = RPC::ERR_CUSTOM_START,

	// Used by anope.listRegisteredChannels.
	ERR_NO_SUCH_DETAIL = RPC::ERR_CUSTOM_START,
};

// Same shape as rpc_data's SaveData: collects the serializable extensible
// items of an object and replies them as a typed map.
class ExtensionData final
	: public Serialize::Data
{
public:
	Anope::unordered_map<Anope::string> data;

	bool LoadInternal(const Anope::string &key, Anope::string &value) override
	{
		return false; // This module can only store data.
	}

	static void Serialize(const Extensible *e, const Serializable *s, RPC::Map &map)
	{
		ExtensionData data;
		Extensible::ExtensibleSerialize(e, s, data);
		for (const auto &[k, v] : data.data)
		{
			switch (data.GetType(k))
			{
				case Serialize::DataType::BOOL:
					map.Reply(k, Anope::Convert<bool>(v, false));
					break;
				case Serialize::DataType::FLOAT:
					map.Reply(k, Anope::Convert<double>(v, 0.0));
					break;
				case Serialize::DataType::INT:
					map.Reply(k, Anope::Convert<int64_t>(v, 0));
					break;
				case Serialize::DataType::TEXT:
				{
					if (v.empty())
						map.Reply(k, nullptr);
					else
						map.Reply(k, v);
					break;
				}
				case Serialize::DataType::UINT:
					map.Reply(k, Anope::Convert<uint64_t>(v, 0));
					break;
			}
		}
	}

	bool StoreInternal(const Anope::string &key, const Anope::string &value) override
	{
		data[key] = value;
		return true;
	}
};

static void ReplySuspension(const SuspendInfo *si, RPC::Map &root)
{
	root.Reply("by", si->by)
		.Reply("time", static_cast<int64_t>(si->when))
		.Reply("expires", static_cast<int64_t>(si->expires));

	if (si->reason.empty())
		root.Reply("reason", nullptr);
	else
		root.Reply("reason", si->reason);
}

class AnopeListRegisteredChannelsRPCEvent final
	: public RPC::Event
{
public:
	AnopeListRegisteredChannelsRPCEvent(Module *o)
		: RPC::Event(o, "anope.listRegisteredChannels")
	{
	}

	static void GetInfo(ChannelInfo *ci, RPC::Map &root)
	{
		root.Reply("name", ci->name)
			.Reply("registered", static_cast<int64_t>(ci->registered))
			.Reply("lastused", static_cast<int64_t>(ci->last_used))
			.Reply("accesscount", static_cast<int64_t>(ci->GetAccessCount()))
			.Reply("users", static_cast<int64_t>(ci->c ? ci->c->users.size() : 0));

		if (const auto *founder = ci->GetFounder())
			root.Reply("founder", founder->display);
		else
			root.Reply("founder", nullptr);

		if (const auto *successor = ci->GetSuccessor())
			root.Reply("successor", successor->display);
		else
			root.Reply("successor", nullptr);

		if (ci->desc.empty())
			root.Reply("description", nullptr);
		else
			root.Reply("description", ci->desc);

		if (ci->last_topic.empty())
			root.Reply("topic", nullptr);
		else
		{
			auto &topic = root.ReplyMap("topic");
			topic.Reply("value", ci->last_topic)
				.Reply("setby", ci->last_topic_setter)
				.Reply("setat", static_cast<int64_t>(ci->last_topic_time));
		}

		if (ci->bi)
			root.Reply("bot", ci->bi->nick);
		else
			root.Reply("bot", nullptr);

		// CSSuspendInfo derives from SuspendInfo first, so the type-erased
		// extensible pointer is the SuspendInfo subobject.
		if (const auto *si = ci->GetExt<SuspendInfo>("CS_SUSPENDED"))
			ReplySuspension(si, root.ReplyMap("suspended"));
		else
			root.Reply("suspended", nullptr);

		ExtensionData::Serialize(ci, ci, root.ReplyMap("extensions"));
	}

	bool Run(RPC::ServiceInterface *iface, HTTP::Client *client, RPC::Request &request) override
	{
		const auto detail = request.data.empty() ? "name" : request.data[0];
		if (detail.equals_ci("name"))
		{
			auto &root = request.Root<RPC::Array>();
			for (auto &[_, ci] : *RegisteredChannelList)
				root.Reply(ci->name);
		}
		else if (detail.equals_ci("full"))
		{
			auto &root = request.Root<RPC::Map>();
			for (auto &[_, ci] : *RegisteredChannelList)
				GetInfo(ci, root.ReplyMap(ci->name));
		}
		else
		{
			request.Error(ERR_NO_SUCH_DETAIL, "No such detail level");
		}
		return true;
	}
};

class AnopeRegisteredChannelRPCEvent final
	: public RPC::Event
{
public:
	AnopeRegisteredChannelRPCEvent(Module *o)
		: RPC::Event(o, "anope.registeredChannel", 1)
	{
	}

	bool Run(RPC::ServiceInterface *iface, HTTP::Client *client, RPC::Request &request) override
	{
		auto *ci = ChannelInfo::Find(request.data[0]);
		if (!ci)
		{
			request.Error(ERR_NO_SUCH_TARGET, "No such channel");
			return true;
		}

		AnopeListRegisteredChannelsRPCEvent::GetInfo(ci, request.Root());
		return true;
	}
};

class AnopeListSuspendedAccountsRPCEvent final
	: public RPC::Event
{
public:
	AnopeListSuspendedAccountsRPCEvent(Module *o)
		: RPC::Event(o, "anope.listSuspendedAccounts")
	{
	}

	bool Run(RPC::ServiceInterface *iface, HTTP::Client *client, RPC::Request &request) override
	{
		auto &root = request.Root<RPC::Map>();
		for (auto &[_, nc] : *NickCoreList)
		{
			// One entry per account: grouped nicks share the core and its
			// suspension. NSSuspendInfo derives from SuspendInfo first.
			if (const auto *si = nc->GetExt<SuspendInfo>("NS_SUSPENDED"))
				ReplySuspension(si, root.ReplyMap(nc->display));
		}
		return true;
	}
};

class ModuleRPCRegistered final
	: public Module
{
private:
	AnopeListRegisteredChannelsRPCEvent anopelistregisteredchannelsrpcevent;
	AnopeRegisteredChannelRPCEvent anoperegisteredchannelrpcevent;
	AnopeListSuspendedAccountsRPCEvent anopelistsuspendedaccountsrpcevent;

public:
	ModuleRPCRegistered(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, EXTRA | VENDOR)
		, anopelistregisteredchannelsrpcevent(this)
		, anoperegisteredchannelrpcevent(this)
		, anopelistsuspendedaccountsrpcevent(this)
	{
	}
};

MODULE_INIT(ModuleRPCRegistered)
