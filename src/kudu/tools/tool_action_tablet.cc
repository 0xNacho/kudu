// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tools/tool_action.h"

#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus_meta.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/sys_catalog.h"
#include "kudu/rpc/messenger.h"
#include "kudu/tserver/tablet_copy_client.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/status.h"

using kudu::consensus::ConsensusMetadata;
using kudu::consensus::RaftConfigPB;
using kudu::consensus::RaftPeerPB;
using kudu::rpc::Messenger;
using kudu::rpc::MessengerBuilder;
using kudu::tserver::TabletCopyClient;
using std::cout;
using std::deque;
using std::endl;
using std::list;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace tools {

namespace {

// Parses a colon-delimited string containing a hostname or IP address and port
// into its respective parts. For example, "localhost:12345" parses into
// hostname=localhost, and port=12345.
//
// Does not allow a port with value 0.
Status ParseHostPortString(const string& hostport_str, HostPort* hostport) {
  HostPort hp;
  Status s = hp.ParseString(hostport_str, 0);
  if (!s.ok()) {
    return s.CloneAndPrepend(Substitute(
        "error while parsing peer '$0'", hostport_str));
  }
  if (hp.port() == 0) {
    return Status::InvalidArgument(
        Substitute("peer '$0' has port of 0", hostport_str));
  }
  *hostport = hp;
  return Status::OK();
}

// Parses a colon-delimited string containing a uuid, hostname or IP address,
// and port into its respective parts. For example,
// "1c7f19e7ecad4f918c0d3d23180fdb18:localhost:12345" parses into
// uuid=1c7f19e7ecad4f918c0d3d23180fdb18, hostname=localhost, and port=12345.
Status ParsePeerString(const string& peer_str,
                       string* uuid,
                       HostPort* hostport) {
  int first_colon_idx = peer_str.find(":");
  if (first_colon_idx == string::npos) {
    return Status::InvalidArgument(Substitute("bad peer '$0'", peer_str));
  }
  string hostport_str = peer_str.substr(first_colon_idx + 1);
  RETURN_NOT_OK(ParseHostPortString(hostport_str, hostport));
  *uuid = peer_str.substr(0, first_colon_idx);
  return Status::OK();
}

Status PrintReplicaUuids(const vector<Mode*>& chain,
                         const Action* action,
                         deque<string> args) {
  // Parse tablet ID argument.
  string tablet_id;
  RETURN_NOT_OK(ParseAndRemoveArg("tablet ID", &args, &tablet_id));
  RETURN_NOT_OK(CheckNoMoreArgs(chain, action, args));

  FsManagerOpts opts;
  opts.read_only = true;
  FsManager fs_manager(Env::Default(), opts);
  RETURN_NOT_OK(fs_manager.Open());

  // Load the cmeta file and print all peer uuids.
  unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK(ConsensusMetadata::Load(&fs_manager, tablet_id,
                                        fs_manager.uuid(), &cmeta));
  cout << JoinMapped(cmeta->committed_config().peers(),
                     [](const RaftPeerPB& p){ return p.permanent_uuid(); },
                     " ") << endl;
  return Status::OK();
}

Status RewriteRaftConfig(const vector<Mode*>& chain,
                         const Action* action,
                         deque<string> args) {
  // Parse tablet ID argument.
  string tablet_id;
  RETURN_NOT_OK(ParseAndRemoveArg("tablet ID", &args, &tablet_id));
  if (tablet_id != master::SysCatalogTable::kSysCatalogTabletId) {
    LOG(WARNING) << "Master will not notice rewritten Raft config of regular "
                 << "tablets. A regular Raft config change must occur.";
  }

  // Parse peer arguments.
  vector<pair<string, HostPort>> peers;
  for (const auto& arg : args) {
    pair<string, HostPort> parsed_peer;
    RETURN_NOT_OK(ParsePeerString(arg,
                                  &parsed_peer.first, &parsed_peer.second));
    peers.push_back(parsed_peer);
  }
  if (peers.empty()) {
    return Status::InvalidArgument(Substitute(
        "must provide at least one peer of form uuid:hostname:port"));
  }

  // Make a copy of the old file before rewriting it.
  Env* env = Env::Default();
  FsManager fs_manager(env, FsManagerOpts());
  RETURN_NOT_OK(fs_manager.Open());
  string cmeta_filename = fs_manager.GetConsensusMetadataPath(tablet_id);
  string backup_filename = Substitute("$0.pre_rewrite.$1",
                                      cmeta_filename, env->NowMicros());
  WritableFileOptions opts;
  opts.mode = Env::CREATE_NON_EXISTING;
  opts.sync_on_close = true;
  RETURN_NOT_OK(env_util::CopyFile(env, cmeta_filename, backup_filename, opts));
  LOG(INFO) << "Backed up current config to " << backup_filename;

  // Load the cmeta file and rewrite the raft config.
  unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK(ConsensusMetadata::Load(&fs_manager, tablet_id,
                                        fs_manager.uuid(), &cmeta));
  RaftConfigPB current_config = cmeta->committed_config();
  RaftConfigPB new_config = current_config;
  new_config.clear_peers();
  for (const auto& p : peers) {
    RaftPeerPB new_peer;
    new_peer.set_member_type(RaftPeerPB::VOTER);
    new_peer.set_permanent_uuid(p.first);
    HostPortPB new_peer_host_port_pb;
    RETURN_NOT_OK(HostPortToPB(p.second, &new_peer_host_port_pb));
    new_peer.mutable_last_known_addr()->CopyFrom(new_peer_host_port_pb);
    new_config.add_peers()->CopyFrom(new_peer);
  }
  cmeta->set_committed_config(new_config);
  return cmeta->Flush();
}

Status Copy(const vector<Mode*>& chain,
            const Action* action,
            deque<string> args) {
  // Parse the tablet ID and source arguments.
  string tablet_id;
  RETURN_NOT_OK(ParseAndRemoveArg("tablet ID", &args, &tablet_id));
  string rpc_address;
  RETURN_NOT_OK(ParseAndRemoveArg("source RPC address of form hostname:port",
                                  &args, &rpc_address));
  RETURN_NOT_OK(CheckNoMoreArgs(chain, action, args));

  HostPort hp;
  RETURN_NOT_OK(ParseHostPortString(rpc_address, &hp));

  // Copy the tablet over.
  FsManager fs_manager(Env::Default(), FsManagerOpts());
  RETURN_NOT_OK(fs_manager.Open());
  MessengerBuilder builder("tablet_copy_client");
  shared_ptr<Messenger> messenger;
  builder.Build(&messenger);
  TabletCopyClient client(tablet_id, &fs_manager, messenger);
  RETURN_NOT_OK(client.Start(hp, nullptr));
  RETURN_NOT_OK(client.FetchAll(nullptr));
  return client.Finish();
}

} // anonymous namespace

unique_ptr<Mode> BuildTabletMode() {
  // TODO: Need to include required arguments in the help for these actions.

  unique_ptr<Action> print_replica_uuids = ActionBuilder(
      { "print_replica_uuids",
        "Print all replica UUIDs found in a tablet's Raft configuration" },
      &PrintReplicaUuids)
    .AddGflag("fs_wal_dir")
    .AddGflag("fs_data_dirs")
    .Build();

  unique_ptr<Action> rewrite_raft_config = ActionBuilder(
      { "rewrite_raft_config", "Rewrite a replica's Raft configuration" },
      &RewriteRaftConfig)
    .AddGflag("fs_wal_dir")
    .AddGflag("fs_data_dirs")
    .Build();

  unique_ptr<Mode> cmeta = ModeBuilder(
      { "cmeta", "Operate on a local Kudu tablet's consensus metadata file" })
    .AddAction(std::move(print_replica_uuids))
    .AddAction(std::move(rewrite_raft_config))
    .Build();

  unique_ptr<Action> copy = ActionBuilder(
      { "copy", "Copy a replica from a remote server" }, &Copy)
    .AddGflag("fs_wal_dir")
    .AddGflag("fs_data_dirs")
    .Build();

  return ModeBuilder({ "tablet", "Operate on a local Kudu replica" })
      .AddMode(std::move(cmeta))
      .AddAction(std::move(copy))
      .Build();
}

} // namespace tools
} // namespace kudu

