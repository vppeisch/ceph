// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "osd.h"

#include <boost/iterator/counting_iterator.hpp>
#include <boost/range/join.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include "common/pick_address.h"

#include "messages/MOSDAlive.h"
#include "messages/MOSDBeacon.h"
#include "messages/MOSDBoot.h"
#include "messages/MOSDMap.h"
#include "messages/MOSDOp.h"
#include "messages/MOSDPGInfo.h"
#include "messages/MOSDPGLog.h"
#include "messages/MOSDPGNotify.h"
#include "messages/MOSDPGQuery.h"
#include "messages/MPGStats.h"
#include "messages/MOSDPGCreate2.h"

#include "crimson/mon/MonClient.h"
#include "crimson/net/Connection.h"
#include "crimson/net/Messenger.h"
#include "crimson/os/cyan_collection.h"
#include "crimson/os/cyan_object.h"
#include "crimson/os/futurized_store.h"
#include "os/Transaction.h"
#include "crimson/osd/heartbeat.h"
#include "crimson/osd/osd_meta.h"
#include "crimson/osd/pg.h"
#include "crimson/osd/pg_backend.h"
#include "crimson/osd/pg_meta.h"
#include "osd/PGPeeringEvent.h"
#include "osd/PeeringState.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_osd);
  }
  static constexpr int TICK_INTERVAL = 1;
}

using ceph::common::local_conf;
using ceph::os::FuturizedStore;

OSD::OSD(int id, uint32_t nonce,
         ceph::net::Messenger& cluster_msgr,
         ceph::net::Messenger& public_msgr,
         ceph::net::Messenger& hb_front_msgr,
         ceph::net::Messenger& hb_back_msgr)
  : whoami{id},
    nonce{nonce},
    beacon_timer{[this] { send_beacon(); }},
    cluster_msgr{cluster_msgr},
    public_msgr{public_msgr},
    monc{new ceph::mon::Client{public_msgr, *this}},
    mgrc{new ceph::mgr::Client{public_msgr, *this}},
    heartbeat{new Heartbeat{*this, *monc, hb_front_msgr, hb_back_msgr}},
    heartbeat_timer{[this] { update_heartbeat_peers(); }},
    store{ceph::os::FuturizedStore::create(
      local_conf().get_val<std::string>("osd_objectstore"),
      local_conf().get_val<std::string>("osd_data"))},
    shard_services{cluster_msgr, public_msgr, *monc, *mgrc, *store}
{
  osdmaps[0] = boost::make_local_shared<OSDMap>();
  for (auto msgr : {std::ref(cluster_msgr), std::ref(public_msgr),
                    std::ref(hb_front_msgr), std::ref(hb_back_msgr)}) {
    msgr.get().set_auth_server(monc.get());
    msgr.get().set_auth_client(monc.get());
  }
}

OSD::~OSD() = default;

namespace {
// Initial features in new superblock.
// Features here are also automatically upgraded
CompatSet get_osd_initial_compat_set()
{
  CompatSet::FeatureSet ceph_osd_feature_compat;
  CompatSet::FeatureSet ceph_osd_feature_ro_compat;
  CompatSet::FeatureSet ceph_osd_feature_incompat;
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_BASE);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_PGINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_OLOC);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_LEC);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_CATEGORIES);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_HOBJECTPOOL);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_BIGINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_LEVELDBINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_LEVELDBLOG);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_SNAPMAPPER);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_HINTS);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_PGMETA);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_MISSING);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_FASTINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_RECOVERY_DELETES);
  return CompatSet(ceph_osd_feature_compat,
                   ceph_osd_feature_ro_compat,
                   ceph_osd_feature_incompat);
}
}

seastar::future<> OSD::mkfs(uuid_d osd_uuid, uuid_d cluster_fsid)
{
  return store->mkfs().then([this] {
    return store->mount();
  }).then([cluster_fsid, osd_uuid, this] {
    superblock.cluster_fsid = cluster_fsid;
    superblock.osd_fsid = osd_uuid;
    superblock.whoami = whoami;
    superblock.compat_features = get_osd_initial_compat_set();

    logger().info(
      "{} writing superblock cluster_fsid {} osd_fsid {}",
      __func__,
      cluster_fsid,
      superblock.osd_fsid);

    meta_coll = make_unique<OSDMeta>(
      store->create_new_collection(coll_t::meta()), store.get());
    ceph::os::Transaction t;
    meta_coll->create(t);
    meta_coll->store_superblock(t, superblock);
    return store->do_transaction(meta_coll->collection(), std::move(t));
  }).then([cluster_fsid, this] {
    store->write_meta("ceph_fsid", cluster_fsid.to_string());
    store->write_meta("whoami", std::to_string(whoami));
    fmt::print("created object store {} for osd.{} fsid {}\n",
               local_conf().get_val<std::string>("osd_data"),
               whoami, cluster_fsid);
    return seastar::now();
  });
}

namespace {
  entity_addrvec_t pick_addresses(int what) {
    entity_addrvec_t addrs;
    CephContext cct;
    if (int r = ::pick_addresses(&cct, what, &addrs, -1); r < 0) {
      throw std::runtime_error("failed to pick address");
    }
    for (auto addr : addrs.v) {
      logger().info("picked address {}", addr);
    }
    return addrs;
  }
  std::pair<entity_addrvec_t, bool>
  replace_unknown_addrs(entity_addrvec_t maybe_unknowns,
                        const entity_addrvec_t& knowns) {
    bool changed = false;
    auto maybe_replace = [&](entity_addr_t addr) {
      if (!addr.is_blank_ip()) {
        return addr;
      }
      for (auto& b : knowns.v) {
        if (addr.get_family() == b.get_family()) {
          auto a = b;
          a.set_nonce(addr.get_nonce());
          a.set_type(addr.get_type());
          a.set_port(addr.get_port());
          changed = true;
          return a;
        }
      }
      throw std::runtime_error("failed to replace unknown address");
    };
    entity_addrvec_t replaced;
    std::transform(maybe_unknowns.v.begin(),
                   maybe_unknowns.v.end(),
                   std::back_inserter(replaced.v),
                   maybe_replace);
    return {replaced, changed};
  }
}

seastar::future<> OSD::start()
{
  logger().info("start");

  return store->mount().then([this] {
    meta_coll = make_unique<OSDMeta>(store->open_collection(coll_t::meta()),
                                     store.get());
    return meta_coll->load_superblock();
  }).then([this](OSDSuperblock&& sb) {
    superblock = std::move(sb);
    return get_map(superblock.current_epoch);
  }).then([this](cached_map_t&& map) {
    shard_services.update_map(osdmap);
    osdmap = std::move(map);
    return load_pgs();
  }).then([this] {
    dispatchers.push_front(this);
    dispatchers.push_front(monc.get());
    dispatchers.push_front(mgrc.get());
    return seastar::when_all_succeed(
      cluster_msgr.try_bind(pick_addresses(CEPH_PICK_ADDRESS_CLUSTER),
                            local_conf()->ms_bind_port_min,
                            local_conf()->ms_bind_port_max)
        .then([this] { return cluster_msgr.start(&dispatchers); }),
      public_msgr.try_bind(pick_addresses(CEPH_PICK_ADDRESS_PUBLIC),
                           local_conf()->ms_bind_port_min,
                           local_conf()->ms_bind_port_max)
        .then([this] { return public_msgr.start(&dispatchers); }));
  }).then([this] {
    return seastar::when_all_succeed(monc->start(),
                                     mgrc->start());
  }).then([this] {
    monc->sub_want("osd_pg_creates", last_pg_create_epoch, 0);
    monc->sub_want("mgrmap", 0, 0);
    monc->sub_want("osdmap", 0, 0);
    return monc->renew_subs();
  }).then([this] {
    if (auto [addrs, changed] =
        replace_unknown_addrs(cluster_msgr.get_myaddrs(),
                              public_msgr.get_myaddrs()); changed) {
      cluster_msgr.set_myaddrs(addrs);
    }
    return heartbeat->start(public_msgr.get_myaddrs(),
                            cluster_msgr.get_myaddrs());
  }).then([this] {
    return start_boot();
  });
}

seastar::future<> OSD::start_boot()
{
  state.set_preboot();
  return monc->get_version("osdmap").then([this](version_t newest, version_t oldest) {
    return _preboot(oldest, newest);
  });
}

seastar::future<> OSD::_preboot(version_t oldest, version_t newest)
{
  logger().info("osd.{}: _preboot", whoami);
  if (osdmap->get_epoch() == 0) {
    logger().warn("waiting for initial osdmap");
  } else if (osdmap->is_destroyed(whoami)) {
    logger().warn("osdmap says I am destroyed");
    // provide a small margin so we don't livelock seeing if we
    // un-destroyed ourselves.
    if (osdmap->get_epoch() > newest - 1) {
      throw std::runtime_error("i am destroyed");
    }
  } else if (osdmap->is_noup(whoami)) {
    logger().warn("osdmap NOUP flag is set, waiting for it to clear");
  } else if (!osdmap->test_flag(CEPH_OSDMAP_SORTBITWISE)) {
    logger().error("osdmap SORTBITWISE OSDMap flag is NOT set; please set it");
  } else if (osdmap->require_osd_release < ceph_release_t::luminous) {
    logger().error("osdmap require_osd_release < luminous; please upgrade to luminous");
  } else if (false) {
    // TODO: update mon if current fullness state is different from osdmap
  } else if (version_t n = local_conf()->osd_map_message_max;
             osdmap->get_epoch() >= oldest - 1 &&
             osdmap->get_epoch() + n > newest) {
    return _send_boot();
  }
  // get all the latest maps
  if (osdmap->get_epoch() + 1 >= oldest) {
    return osdmap_subscribe(osdmap->get_epoch() + 1, false);
  } else {
    return osdmap_subscribe(oldest - 1, true);
  }
}

seastar::future<> OSD::_send_boot()
{
  state.set_booting();

  logger().info("hb_back_msgr: {}", heartbeat->get_back_addrs());
  logger().info("hb_front_msgr: {}", heartbeat->get_front_addrs());
  logger().info("cluster_msgr: {}", cluster_msgr.get_myaddr());
  auto m = make_message<MOSDBoot>(superblock,
                                  osdmap->get_epoch(),
                                  osdmap->get_epoch(),
                                  heartbeat->get_back_addrs(),
                                  heartbeat->get_front_addrs(),
                                  cluster_msgr.get_myaddrs(),
                                  CEPH_FEATURES_ALL);
  return monc->send_message(m);
}

seastar::future<> OSD::_send_alive()
{
  auto want = osdmap->get_epoch();
  logger().info(
    "{} want {} up_thru_wanted {}",
    __func__,
    want,
    up_thru_wanted);
  if (!osdmap->exists(whoami)) {
    return seastar::now();
  } else if (want <= up_thru_wanted){
    return seastar::now();
  } else {
    up_thru_wanted = want;
    auto m = make_message<MOSDAlive>(osdmap->get_epoch(), want);
    return monc->send_message(std::move(m));
  }
}

seastar::future<> OSD::stop()
{
  logger().info("stop");
  // see also OSD::shutdown()
  state.set_stopping();
  return gate.close().then([this] {
    return heartbeat->stop();
  }).then([this] {
    return monc->stop();
  }).then([this] {
    return store->umount();
  });
}

seastar::future<> OSD::load_pgs()
{
  return seastar::parallel_for_each(store->list_collections(),
    [this](auto coll) {
      spg_t pgid;
      if (coll.is_pg(&pgid)) {
        return load_pg(pgid).then([pgid, this](auto&& pg) {
          logger().info("load_pgs: loaded {}", pgid);
          pgs.emplace(pgid, std::move(pg));
          return seastar::now();
        });
      } else if (coll.is_temp(&pgid)) {
        // TODO: remove the collection
        return seastar::now();
      } else {
        logger().warn("ignoring unrecognized collection: {}", coll);
        return seastar::now();
      }
    });
}

seastar::future<Ref<PG>> OSD::make_pg(cached_map_t create_map, spg_t pgid)
{
  using ec_profile_t = map<string,string>;
  return ([&]() {
    if (create_map->have_pg_pool(pgid.pool())) {
      pg_pool_t pi = *create_map->get_pg_pool(pgid.pool());
      string name = create_map->get_pool_name(pgid.pool());
      ec_profile_t ec_profile;
      if (pi.is_erasure()) {
	ec_profile = create_map->get_erasure_code_profile(pi.erasure_code_profile);
      }
      return seastar::make_ready_future<pg_pool_t, string, ec_profile_t>(
	std::move(pi),
	std::move(name),
	std::move(ec_profile));
    } else {
      // pool was deleted; grab final pg_pool_t off disk.
      return meta_coll->load_final_pool_info(pgid.pool());
    }
  })().then([pgid, this, create_map](pg_pool_t&& pool,
                       string&& name,
                       ec_profile_t&& ec_profile) {
    return seastar::make_ready_future<Ref<PG>>(Ref<PG>{new PG{pgid,
	    pg_shard_t{whoami, pgid.shard},
	    std::move(pool),
	    std::move(name),
	    create_map,
	    shard_services,
	    ec_profile}});
  });
}

seastar::future<Ref<PG>> OSD::load_pg(spg_t pgid)
{
  return PGMeta{store.get(), pgid}.get_epoch().then([this](epoch_t e) {
    return get_map(e);
  }).then([pgid, this] (auto&& create_map) {
    return make_pg(std::move(create_map), pgid);
  }).then([this, pgid](Ref<PG> pg) {
    return pg->read_state(store.get()).then([pg] {
      return seastar::make_ready_future<Ref<PG>>(std::move(pg));
    }).handle_exception([pgid](auto ep) {
      logger().info("pg {} saw exception on load {}", pgid, ep);
      ceph_abort("Could not load pg" == 0);
      return seastar::make_exception_future<Ref<PG>>(ep);
    });
  });
}

seastar::future<> OSD::ms_dispatch(ceph::net::Connection* conn, MessageRef m)
{
  if (state.is_stopping()) {
    return seastar::now();
  }

  switch (m->get_type()) {
  case CEPH_MSG_OSD_MAP:
    return handle_osd_map(conn, boost::static_pointer_cast<MOSDMap>(m));
  case CEPH_MSG_OSD_OP:
    return handle_osd_op(conn, boost::static_pointer_cast<MOSDOp>(m));
  case MSG_OSD_PG_NOTIFY:
    return handle_pg_notify(conn, boost::static_pointer_cast<MOSDPGNotify>(m));
  case MSG_OSD_PG_INFO:
    return handle_pg_info(conn, boost::static_pointer_cast<MOSDPGInfo>(m));
  case MSG_OSD_PG_QUERY:
    return handle_pg_query(conn, boost::static_pointer_cast<MOSDPGQuery>(m));
  case MSG_OSD_PG_LOG:
    return handle_pg_log(conn, boost::static_pointer_cast<MOSDPGLog>(m));
  case MSG_OSD_PG_CREATE2:
    return handle_pg_create(conn, boost::static_pointer_cast<MOSDPGCreate2>(m));
  default:
    logger().info("{} unhandled message {}", __func__, *m);
    return seastar::now();
  }
}

seastar::future<> OSD::ms_handle_connect(ceph::net::ConnectionRef conn)
{
  if (conn->get_peer_type() != CEPH_ENTITY_TYPE_MON) {
    return seastar::now();
  } else {
    return seastar::now();
  }
}

seastar::future<> OSD::ms_handle_reset(ceph::net::ConnectionRef conn)
{
  // TODO: cleanup the session attached to this connection
  logger().warn("ms_handle_reset");
  return seastar::now();
}

seastar::future<> OSD::ms_handle_remote_reset(ceph::net::ConnectionRef conn)
{
  logger().warn("ms_handle_remote_reset");
  return seastar::now();
}

void OSD::handle_authentication(const EntityName& name,
                                 uint64_t global_id,
                                 const AuthCapsInfo& caps)
{
  // todo
}

MessageRef OSD::get_stats()
{
  // todo: m-to-n: collect stats using map-reduce
  // MPGStats::had_map_for is not used since PGMonitor was removed
  auto m = make_message<MPGStats>(monc->get_fsid(), osdmap->get_epoch());

  for (auto [pgid, pg] : pgs) {
    if (pg->is_primary()) {
      auto stats = pg->get_stats();
      // todo: update reported_epoch,reported_seq,last_fresh
      stats.reported_epoch = osdmap->get_epoch();
      m->pg_stat.emplace(pgid.pgid, std::move(stats));
    }
  }
  return m;
}

OSD::cached_map_t OSD::get_map() const
{
  return osdmap;
}

seastar::future<OSD::cached_map_t> OSD::get_map(epoch_t e)
{
  // TODO: use LRU cache for managing osdmap, fallback to disk if we have to
  if (auto found = osdmaps.find(e); found) {
    return seastar::make_ready_future<cached_map_t>(std::move(found));
  } else {
    return load_map(e).then([e, this](unique_ptr<OSDMap> osdmap) {
      return seastar::make_ready_future<cached_map_t>(
        osdmaps.insert(e, std::move(osdmap)));
    });
  }
}

void OSD::store_map_bl(ceph::os::Transaction& t,
                       epoch_t e, bufferlist&& bl)
{
  meta_coll->store_map(t, e, bl);
  map_bl_cache.insert(e, std::move(bl));
}

seastar::future<bufferlist> OSD::load_map_bl(epoch_t e)
{
  if (std::optional<bufferlist> found = map_bl_cache.find(e); found) {
    return seastar::make_ready_future<bufferlist>(*found);
  } else {
    return meta_coll->load_map(e);
  }
}

seastar::future<std::unique_ptr<OSDMap>> OSD::load_map(epoch_t e)
{
  auto o = std::make_unique<OSDMap>();
  if (e > 0) {
    return load_map_bl(e).then([e, o=std::move(o), this](bufferlist bl) mutable {
      o->decode(bl);
      return seastar::make_ready_future<unique_ptr<OSDMap>>(std::move(o));
    });
  } else {
    return seastar::make_ready_future<unique_ptr<OSDMap>>(std::move(o));
  }
}

seastar::future<> OSD::store_maps(ceph::os::Transaction& t,
                                  epoch_t start, Ref<MOSDMap> m)
{
  return seastar::do_for_each(boost::make_counting_iterator(start),
                              boost::make_counting_iterator(m->get_last() + 1),
                              [&t, m, this](epoch_t e) {
    if (auto p = m->maps.find(e); p != m->maps.end()) {
      auto o = std::make_unique<OSDMap>();
      o->decode(p->second);
      logger().info("store_maps osdmap.{}", e);
      store_map_bl(t, e, std::move(std::move(p->second)));
      osdmaps.insert(e, std::move(o));
      return seastar::now();
    } else if (auto p = m->incremental_maps.find(e);
               p != m->incremental_maps.end()) {
      return load_map(e - 1).then([e, bl=p->second, &t, this](auto o) {
        OSDMap::Incremental inc;
        auto i = bl.cbegin();
        inc.decode(i);
        o->apply_incremental(inc);
        bufferlist fbl;
        o->encode(fbl, inc.encode_features | CEPH_FEATURE_RESERVED);
        store_map_bl(t, e, std::move(fbl));
        osdmaps.insert(e, std::move(o));
        return seastar::now();
      });
    } else {
      logger().error("MOSDMap lied about what maps it had?");
      return seastar::now();
    }
  });
}

seastar::future<> OSD::osdmap_subscribe(version_t epoch, bool force_request)
{
  logger().info("{}({})", __func__, epoch);
  if (monc->sub_want_increment("osdmap", epoch, CEPH_SUBSCRIBE_ONETIME) ||
      force_request) {
    return monc->renew_subs();
  } else {
    return seastar::now();
  }
}

bool OSD::require_mon_peer(ceph::net::Connection *conn, Ref<Message> m)
{
  if (!conn->peer_is_mon()) {
    logger().info("{} received from non-mon {}, {}",
		  __func__,
		  conn->get_peer_addr(),
		  *m);
    return false;
  }
  return true;
}

seastar::future<Ref<PG>> OSD::handle_pg_create_info(
  std::unique_ptr<PGCreateInfo> info) {
  return seastar::do_with(
    std::move(info),
    [this](auto &info) -> seastar::future<Ref<PG>> {
      return get_map(info->epoch).then(
	[&info, this](cached_map_t startmap) ->
	seastar::future<Ref<PG>, cached_map_t> {
	  const spg_t &pgid = info->pgid;
	  if (info->by_mon) {
	    int64_t pool_id = pgid.pgid.pool();
	    const pg_pool_t *pool = osdmap->get_pg_pool(pool_id);
	    if (!pool) {
	      logger().debug(
		"{} ignoring pgid {}, pool dne",
		__func__,
		pgid);
	      return seastar::make_ready_future<Ref<PG>, cached_map_t>(
		Ref<PG>(),
		startmap);
	    }
	    ceph_assert(osdmap->require_osd_release >= ceph_release_t::nautilus);
	    if (!pool->has_flag(pg_pool_t::FLAG_CREATING)) {
	      // this ensures we do not process old creating messages after the
	      // pool's initial pgs have been created (and pg are subsequently
	      // allowed to split or merge).
	      logger().debug(
		"{} dropping {} create, pool does not have CREATING flag set",
		__func__,
		pgid);
	      return seastar::make_ready_future<Ref<PG>, cached_map_t>(
		Ref<PG>(),
		startmap);
	    }
	  }
	  return make_pg(startmap, pgid).then(
	    [this, startmap=std::move(startmap)](auto pg) mutable {
	      return seastar::make_ready_future<Ref<PG>, cached_map_t>(
		std::move(pg),
		std::move(startmap));
	    });
      }).then(
	[this, &info](auto pg, auto startmap) -> seastar::future<Ref<PG>> {
	  if (!pg)
	    return seastar::make_ready_future<Ref<PG>>(Ref<PG>());
	  PeeringCtx rctx;
	  const pg_pool_t* pp = startmap->get_pg_pool(info->pgid.pool());

	  int up_primary, acting_primary;
	  vector<int> up, acting;
	  startmap->pg_to_up_acting_osds(
	    info->pgid.pgid, &up, &up_primary, &acting, &acting_primary);

	  int role = startmap->calc_pg_role(whoami, acting, acting.size());
	  if (!pp->is_replicated() && role != info->pgid.shard) {
	    role = -1;
	  }


	  auto coll = store->create_new_collection(coll_t(info->pgid));
	  create_pg_collection(
	    rctx.transaction,
	    info->pgid,
	    info->pgid.get_split_bits(pp->get_pg_num()));
	  init_pg_ondisk(
	    rctx.transaction,
	    info->pgid,
	    pp);

	  pg->init(
	    coll,
	    role,
	    up,
	    up_primary,
	    acting,
	    acting_primary,
	    info->history,
	    info->past_intervals,
	    false,
	    rctx.transaction);

	  pg->handle_initialize(rctx);
	  pg->handle_activate_map(rctx);

	  logger().info("{} new pg {}", __func__, *pg);
	  pgs.emplace(info->pgid, pg);
	  return seastar::when_all_succeed(
	    pg->get_need_up_thru() ? _send_alive() : seastar::now(),
	    shard_services.dispatch_context(
	      pg->get_collection_ref(),
	      std::move(rctx)).then(
		[pg]() { return seastar::make_ready_future<Ref<PG>>(pg); }));
	});
    });
}

seastar::future<> OSD::handle_pg_create(
  ceph::net::Connection* conn,
  Ref<MOSDPGCreate2> m)
{
  logger().info("{}: {} from {}", __func__, *m, m->get_source());
  if (!require_mon_peer(conn, m)) {
    return seastar::now();
  }
  return handle_batch_pg_message(
    m->pgs,
    [this, conn, m](auto p)
    -> std::optional<std::tuple<spg_t, std::unique_ptr<PGPeeringEvent>>> {
      const spg_t &pgid = p.first;
      const auto &[created, created_stamp] = p.second;

      auto q = m->pg_extra.find(pgid);
      ceph_assert(q != m->pg_extra.end());
      logger().debug(
	"{} {} e{} @{} history {} pi {}",
	__func__,
	pgid,
	created,
	created_stamp,
	q->second.first,
	q->second.second);
      if (!q->second.second.empty() &&
	  m->epoch < q->second.second.get_bounds().second) {
	logger().error(
	  "got pg_create on {} epoch {} unmatched past_intervals (history {})",
	  pgid,
	  m->epoch,
	  q->second.second,
	  q->second.first);
	return std::nullopt;
      } else {
	return std::make_optional(
	  std::make_tuple(
	    pgid,
	    std::make_unique<PGPeeringEvent>(
	      m->epoch,
	      m->epoch,
	      NullEvt(),
	      true,
	      new PGCreateInfo(
		pgid,
		m->epoch,
		q->second.first,
		q->second.second,
		true))));
      }
    });
}

seastar::future<> OSD::handle_osd_map(ceph::net::Connection* conn,
                                      Ref<MOSDMap> m)
{
  logger().info("handle_osd_map {}", *m);
  if (m->fsid != superblock.cluster_fsid) {
    logger().warn("fsid mismatched");
    return seastar::now();
  }
  if (state.is_initializing()) {
    logger().warn("i am still initializing");
    return seastar::now();
  }

  const auto first = m->get_first();
  const auto last = m->get_last();
  logger().info("handle_osd_map epochs [{}..{}], i have {}, src has [{}..{}]",
                first, last, superblock.newest_map, m->oldest_map, m->newest_map);
  // make sure there is something new, here, before we bother flushing
  // the queues and such
  if (last <= superblock.newest_map) {
    return seastar::now();
  }
  // missing some?
  bool skip_maps = false;
  epoch_t start = superblock.newest_map + 1;
  if (first > start) {
    logger().info("handle_osd_map message skips epochs {}..{}",
                  start, first - 1);
    if (m->oldest_map <= start) {
      return osdmap_subscribe(start, false);
    }
    // always try to get the full range of maps--as many as we can.  this
    //  1- is good to have
    //  2- is at present the only way to ensure that we get a *full* map as
    //     the first map!
    if (m->oldest_map < first) {
      return osdmap_subscribe(m->oldest_map - 1, true);
    }
    skip_maps = true;
    start = first;
  }

  return seastar::do_with(ceph::os::Transaction{},
                          [=](auto& t) {
    return store_maps(t, start, m).then([=, &t] {
      // even if this map isn't from a mon, we may have satisfied our subscription
      monc->sub_got("osdmap", last);
      if (!superblock.oldest_map || skip_maps) {
        superblock.oldest_map = first;
      }
      superblock.newest_map = last;
      superblock.current_epoch = last;

      // note in the superblock that we were clean thru the prior epoch
      if (boot_epoch && boot_epoch >= superblock.mounted) {
        superblock.mounted = boot_epoch;
        superblock.clean_thru = last;
      }
      meta_coll->store_superblock(t, superblock);
      return store->do_transaction(meta_coll->collection(), std::move(t));
    });
  }).then([=] {
    // TODO: write to superblock and commit the transaction
    return committed_osd_maps(start, last, m);
  });
}

seastar::future<> OSD::committed_osd_maps(version_t first,
                                          version_t last,
                                          Ref<MOSDMap> m)
{
  logger().info("osd.{}: committed_osd_maps({}, {})", whoami, first, last);
  // advance through the new maps
  return seastar::do_for_each(boost::make_counting_iterator(first),
                              boost::make_counting_iterator(last + 1),
                              [this](epoch_t cur) {
    return get_map(cur).then([this](cached_map_t&& o) {
      osdmap = std::move(o);
      shard_services.update_map(osdmap);
      if (up_epoch != 0 &&
          osdmap->is_up(whoami) &&
          osdmap->get_addrs(whoami) == public_msgr.get_myaddrs()) {
        up_epoch = osdmap->get_epoch();
        if (!boot_epoch) {
          boot_epoch = osdmap->get_epoch();
        }
      }
    });
  }).then([m, this] {
    if (osdmap->is_up(whoami) &&
        osdmap->get_addrs(whoami) == public_msgr.get_myaddrs() &&
        bind_epoch < osdmap->get_up_from(whoami)) {
      if (state.is_booting()) {
        logger().info("osd.{}: activating...", whoami);
        state.set_active();
        beacon_timer.arm_periodic(
          std::chrono::seconds(local_conf()->osd_beacon_report_interval));
        heartbeat_timer.arm_periodic(
          std::chrono::seconds(TICK_INTERVAL));
      }
    }
    check_osdmap_features();
    // yay!
    return consume_map(osdmap->get_epoch());
  }).then([m, this] {
    if (state.is_active()) {
      logger().info("osd.{}: now active", whoami);
      if (!osdmap->exists(whoami)) {
        return shutdown();
      }
      if (should_restart()) {
        return restart();
      } else {
        return seastar::now();
      }
    } else if (state.is_preboot()) {
      logger().info("osd.{}: now preboot", whoami);

      if (m->get_source().is_mon()) {
        return _preboot(m->oldest_map, m->newest_map);
      } else {
        logger().info("osd.{}: start_boot", whoami);
        return start_boot();
      }
    } else {
      logger().info("osd.{}: now {}", whoami, state);
      // XXX
      return seastar::now();
    }
  });
}

seastar::future<> OSD::handle_osd_op(ceph::net::Connection* conn,
                                     Ref<MOSDOp> m)
{
  return wait_for_map(m->get_map_epoch()).then([=](epoch_t epoch) {
    if (auto found = pgs.find(m->get_spg()); found != pgs.end()) {
      return found->second->handle_op(conn, std::move(m));
    } else if (osdmap->is_up_acting_osd_shard(m->get_spg(), whoami)) {
      logger().info("no pg, should exist e{}, will wait", epoch);
      // todo, wait for peering, etc
      return seastar::now();
    } else {
      logger().info("no pg, shouldn't exist e{}, dropping", epoch);
      // todo: share map with client
      return seastar::now();
    }
  });
}

bool OSD::should_restart() const
{
  if (!osdmap->is_up(whoami)) {
    logger().info("map e {} marked osd.{} down",
                  osdmap->get_epoch(), whoami);
    return true;
  } else if (osdmap->get_addrs(whoami) != public_msgr.get_myaddrs()) {
    logger().error("map e {} had wrong client addr ({} != my {})",
                   osdmap->get_epoch(),
                   osdmap->get_addrs(whoami),
                   public_msgr.get_myaddrs());
    return true;
  } else if (osdmap->get_cluster_addrs(whoami) != cluster_msgr.get_myaddrs()) {
    logger().error("map e {} had wrong cluster addr ({} != my {})",
                   osdmap->get_epoch(),
                   osdmap->get_cluster_addrs(whoami),
                   cluster_msgr.get_myaddrs());
    return true;
  } else {
    return false;
  }
}

seastar::future<> OSD::restart()
{
  beacon_timer.cancel();
  heartbeat_timer.cancel();
  up_epoch = 0;
  bind_epoch = osdmap->get_epoch();
  // TODO: promote to shutdown if being marked down for multiple times
  // rebind messengers
  return start_boot();
}

seastar::future<> OSD::shutdown()
{
  // TODO
  superblock.mounted = boot_epoch;
  superblock.clean_thru = osdmap->get_epoch();
  return seastar::now();
}

seastar::future<> OSD::send_beacon()
{
  // FIXME: min lec should be calculated from pg_stat
  //        and should set m->pgs
  epoch_t min_last_epoch_clean = osdmap->get_epoch();
  auto m = make_message<MOSDBeacon>(osdmap->get_epoch(),
                                    min_last_epoch_clean);
  return monc->send_message(m);
}

void OSD::update_heartbeat_peers()
{
  if (!state.is_active()) {
    return;
  }
  for (auto& pg : pgs) {
    vector<int> up, acting;
    osdmap->pg_to_up_acting_osds(pg.first.pgid,
                                 &up, nullptr,
                                 &acting, nullptr);
    for (auto osd : boost::join(up, acting)) {
      if (osd != CRUSH_ITEM_NONE && osd != whoami) {
        heartbeat->add_peer(osd, osdmap->get_epoch());
      }
    }
  }
  heartbeat->update_peers(whoami);
}

seastar::future<> OSD::handle_pg_notify(
  ceph::net::Connection* conn,
  Ref<MOSDPGNotify> m)
{
  // assuming all pgs reside in a single shard
  // see OSD::dequeue_peering_evt()
  const int from = m->get_source().num();
  return handle_batch_pg_message(
    m->get_pg_list(),
    [from, this](pair<pg_notify_t, PastIntervals> p) {
      auto& [pg_notify, past_intervals] = p;
      spg_t pgid{pg_notify.info.pgid.pgid, pg_notify.to};
      MNotifyRec notify{pgid,
                        pg_shard_t{from, pg_notify.from},
                        pg_notify,
                        0, // the features is not used
                        past_intervals};
      logger().debug("handle_pg_notify on {} from {}", pgid.pgid, from);
      auto create_info = new PGCreateInfo{
	pgid,
	pg_notify.query_epoch,
	pg_notify.info.history,
	past_intervals,
	false};
      return std::make_optional(
	std::make_tuple(
	  pgid,
	  std::make_unique<PGPeeringEvent>(
	    pg_notify.epoch_sent,
	    pg_notify.query_epoch,
	    notify,
	    true, // requires_pg
	    create_info)));
  });
}

seastar::future<> OSD::handle_pg_info(
  ceph::net::Connection* conn,
  Ref<MOSDPGInfo> m)
{
  // assuming all pgs reside in a single shard
  // see OSD::dequeue_peering_evt()
  const int from = m->get_source().num();
  return handle_batch_pg_message(
    m->pg_list,
    [from, this](pair<pg_notify_t, PastIntervals> p) {
      auto& pg_notify = p.first;
      spg_t pgid{pg_notify.info.pgid.pgid, pg_notify.to};
      logger().debug("handle_pg_info on {} from {}", pgid.pgid, from);
      MInfoRec info{pg_shard_t{from, pg_notify.from},
                    pg_notify.info,
                    pg_notify.epoch_sent};
      return std::make_optional(
	std::tuple(
	  pgid,
	  std::make_unique<PGPeeringEvent>(
	    pg_notify.epoch_sent,
	    pg_notify.query_epoch,
	    std::move(info))));
    });
}

seastar::future<> OSD::handle_pg_query(ceph::net::Connection* conn,
                                       Ref<MOSDPGQuery> m)
{
  // assuming all pgs reside in a single shard
  // see OSD::dequeue_peering_evt()
  const int from = m->get_source().num();
  // TODO: handle missing pg -- handle_batch_pg_message ignores pgs
  // that don't exist
  return handle_batch_pg_message_with_missing_handler(
    m->pg_list,
    [from, this](pair<spg_t, pg_query_t> p) {
      auto& [pgid, pg_query] = p;
      MQuery query{pgid, pg_shard_t{from, pg_query.from},
		   pg_query, pg_query.epoch_sent};
      logger().debug("handle_pg_query on {} from {}", pgid, from);
      return std::make_optional(
	std::make_tuple(
	  pgid,
	  std::make_unique<PGPeeringEvent>(
	    pg_query.epoch_sent,
	    pg_query.epoch_sent,
	    std::move(query))));
    },
    [this, from](pair<spg_t, pg_query_t> p, PeeringCtx &ctx) {
      auto &[pgid, query] = p;
      logger().debug("handle_pg_query on absent pg {} from {}", pgid, from);
      pg_info_t empty(spg_t(pgid.pgid, query.to));
      ceph_assert(query.type == pg_query_t::INFO);
      ctx.notify_list[from].emplace_back(
        pg_notify_t(
          query.from, query.to,
          query.epoch_sent,
	  osdmap->get_epoch(),
	  empty),
	PastIntervals());
    });
}

seastar::future<> OSD::handle_pg_log(
  ceph::net::Connection* conn,
  Ref<MOSDPGLog> m)
{
  const int from = m->get_source().num();
  logger().debug("handle_pg_log on {} from {}", m->get_spg(), from);
  return do_peering_event_and_dispatch(
    m->get_spg(),
    PGPeeringEventURef(m->get_event()));
}

void OSD::check_osdmap_features()
{
  if (osdmap->require_osd_release < ceph_release_t::nautilus) {
    heartbeat->set_require_authorizer(false);
  } else {
    heartbeat->set_require_authorizer(true);
  }
}

seastar::future<> OSD::consume_map(epoch_t epoch)
{
  // todo: m-to-n: broadcast this news to all shards
  return seastar::parallel_for_each(pgs.begin(), pgs.end(), [=](auto& pg) {
    return advance_pg_to(pg.second, epoch);
  }).then([epoch, this] {
    auto first = waiting_peering.begin();
    auto last = waiting_peering.upper_bound(epoch);
    std::for_each(first, last, [epoch, this](auto& blocked_requests) {
      blocked_requests.second.set_value(epoch);
    });
    waiting_peering.erase(first, last);
    return seastar::now();
  });
}


seastar::future<Ref<PG>>
OSD::get_pg(
  spg_t pgid,
  epoch_t epoch,
  std::unique_ptr<PGCreateInfo> info)
{
  return wait_for_map(epoch).then([this, pgid, epoch, info=std::move(info)](epoch_t) mutable {
    if (auto pg = pgs.find(pgid); pg != pgs.end()) {
      return advance_pg_to(pg->second, epoch).then([pg=pg->second]() {
	return seastar::make_ready_future<Ref<PG>>(pg);
      });
    } else if (!info) {
      return seastar::make_ready_future<Ref<PG>>();
    } else {
      auto creating = pgs_creating.find(pgid);
      if (creating == pgs_creating.end()) {
	creating = pgs_creating.emplace(
	  pgid,
	  seastar::shared_future<Ref<PG>>(handle_pg_create_info(std::move(info)).then([this, pgid](auto pg) {
	    pgs_creating.erase(pgid);
	    return seastar::make_ready_future<Ref<PG>>(pg);
	  }))).first;
      }
      return creating->second.get_future().then([this, epoch](auto pg) {
	return advance_pg_to(pg, epoch).then([pg]() {
	  return seastar::make_ready_future<Ref<PG>>(pg);
	});
      });
    }
  });
}

seastar::future<Ref<PG>>
OSD::do_peering_event(
  spg_t pgid,
  PGPeeringEventURef evt,
  PeeringCtx &rctx)
{
  return get_pg(pgid, evt->get_epoch_sent(), std::move(evt->create_info))
    .then([this, evt=std::move(evt), &rctx](Ref<PG> pg) mutable {
      if (pg) {
	pg->do_peering_event(std::move(evt), rctx);
      }
      return seastar::make_ready_future<Ref<PG>>(pg);
    });
}

seastar::future<bool>
OSD::do_peering_event_and_dispatch_transaction(
  spg_t pgid,
  std::unique_ptr<PGPeeringEvent> evt,
  PeeringCtx &rctx)
{
  return do_peering_event(pgid, std::move(evt), rctx).then(
    [this, pgid, &rctx](Ref<PG> pg) mutable {
      if (pg) {
	return seastar::when_all_succeed(
	  pg->get_need_up_thru() ? _send_alive() : seastar::now(),
	  shard_services.dispatch_context_transaction(
	    pg->get_collection_ref(), rctx)).then([] { return true; });
      } else {
	return seastar::make_ready_future<bool>(false);
      }
    });
}

seastar::future<>
OSD::do_peering_event_and_dispatch(
  spg_t pgid,
  std::unique_ptr<PGPeeringEvent> evt)
{
  return seastar::do_with(
    PeeringCtx{},
    [this, pgid, evt=std::move(evt)](auto &rctx) mutable {
      return do_peering_event(pgid, std::move(evt), rctx).then(
	[this, pgid, &rctx](Ref<PG> pg) mutable {
	  if (pg) {
	    return seastar::when_all_succeed(
	      pg->get_need_up_thru() ? _send_alive() : seastar::now(),
	      shard_services.dispatch_context(
		pg->get_collection_ref(), std::move(rctx)));
	  } else {
	    return seastar::now();
	  }
	});
    }).handle_exception([](auto ep) {
      logger().error("do_peering_event_and_dispatch saw {}", ep);
      return seastar::make_exception_future<>(ep);
    });
}

seastar::future<epoch_t> OSD::wait_for_map(epoch_t epoch)
{
  const auto mine = osdmap->get_epoch();
  if (mine >= epoch) {
    return seastar::make_ready_future<epoch_t>(mine);
  } else {
    logger().info("evt epoch is {}, i have {}, will wait", epoch, mine);
    auto fut = waiting_peering[epoch].get_shared_future();
    return osdmap_subscribe(osdmap->get_epoch(), true).then(
      [fut=std::move(fut)]() mutable {
      return std::move(fut);
    });
  }
}

seastar::future<> OSD::advance_pg_to(Ref<PG> pg, epoch_t to)
{
  auto from = pg->get_osdmap_epoch();
  // todo: merge/split support
  return seastar::do_with(
    PeeringCtx{},
    [this, pg, from, to](auto &rctx) {
      return seastar::do_for_each(
	boost::make_counting_iterator(from + 1),
	boost::make_counting_iterator(to + 1),
	[this, pg, &rctx](epoch_t next_epoch) {
	  return get_map(next_epoch).then(
	    [pg, this, &rctx] (cached_map_t&& next_map) {
	      return pg->handle_advance_map(next_map, rctx);
	    });
	}).then([this, &rctx, pg] {
	  pg->handle_activate_map(rctx);
	  return seastar::when_all_succeed(
	    pg->get_need_up_thru() ? _send_alive() : seastar::now(),
	    shard_services.dispatch_context(
	      pg->get_collection_ref(),
	      std::move(rctx)));
	});
    });
}
