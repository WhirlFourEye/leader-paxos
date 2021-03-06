#include "paxos_worker.h"
#include "service.h"

namespace janus {

static int volatile xx =
    MarshallDeputy::RegInitializer(MarshallDeputy::CONTAINER_CMD,
                                   []() -> Marshallable* {
                                     return new LogEntry;
                                   });

Marshal& LogEntry::ToMarshal(Marshal& m) const {
  m << length;
  m << std::string(operation_);
  return m;
};

Marshal& LogEntry::FromMarshal(Marshal& m) {
  m >> length;
  std::string str;
  m >> str;
  operation_ = new char[length];
  strcpy(operation_, str.c_str());
  return m;
};

void PaxosWorker::SetupBase() {
  auto config = Config::GetConfig();
  rep_frame_ = Frame::GetFrame(config->replica_proto_);
  rep_frame_->site_info_ = site_info_;
  rep_sched_ = rep_frame_->CreateScheduler();
  // rep_sched_->txn_reg_ = tx_reg_;
  rep_sched_->loc_id_ = site_info_->locale_id;
}

void PaxosWorker::Next(Marshallable& cmd) {
  // if (IsLeader()) {
  gettimeofday(&commit_time_, NULL);
  // }
  if (cmd.kind_ == MarshallDeputy::CONTAINER_CMD) {
    if (this->callback_ != nullptr) {
      auto& sp_log_entry = dynamic_cast<LogEntry&>(cmd);
      callback_(sp_log_entry.operation_, sp_log_entry.length);
    }
  } else {
    verify(0);
  }
  finish_mutex.lock();
  if (n_current > 0) {
    n_current--;
    if (n_current == 0) finish_cond.signal();
  }
  finish_mutex.unlock();
}

void PaxosWorker::SetupService() {
  std::string bind_addr = site_info_->GetBindAddress();
  int n_io_threads = 1;
  svr_poll_mgr_ = new rrr::PollMgr(n_io_threads);
  if (rep_frame_ != nullptr) {
    services_ = rep_frame_->CreateRpcServices(site_info_->id,
                                              rep_sched_,
                                              svr_poll_mgr_,
                                              scsi_);
  }
  uint32_t num_threads = 1;
  thread_pool_g = new base::ThreadPool(num_threads);

  // init rrr::Server
  rpc_server_ = new rrr::Server(svr_poll_mgr_, thread_pool_g);

  // reg services
  for (auto service : services_) {
    rpc_server_->reg(service);
  }

  // start rpc server
  Log_debug("starting server at %s", bind_addr.c_str());
  int ret = rpc_server_->start(bind_addr.c_str());
  if (ret != 0) {
    Log_fatal("server launch failed.");
  }

  Log_info("Server %s ready at %s",
           site_info_->name.c_str(),
           bind_addr.c_str());
}

void PaxosWorker::SetupCommo() {
  if (rep_frame_) {
    rep_commo_ = rep_frame_->CreateCommo();
    if (rep_commo_) {
      rep_commo_->loc_id_ = site_info_->locale_id;
    }
    rep_sched_->commo_ = rep_commo_;
  }
}

void PaxosWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  scsi_ = new ServerControlServiceImpl(timeout);
  int n_io_threads = 1;
  svr_hb_poll_mgr_g = new rrr::PollMgr(n_io_threads);
  hb_thread_pool_g = new rrr::ThreadPool(1);
  hb_rpc_server_ = new rrr::Server(svr_hb_poll_mgr_g, hb_thread_pool_g);
  hb_rpc_server_->reg(scsi_);

  auto port = site_info_->port + CtrlPortDelta;
  std::string addr_port = std::string("0.0.0.0:") +
                          std::to_string(port);
  hb_rpc_server_->start(addr_port.c_str());
  if (hb_rpc_server_ != nullptr) {
    // Log_info("notify ready to control script for %s", bind_addr.c_str());
    scsi_->set_ready();
  }
  Log_info("heartbeat setup for %s on %s",
           site_info_->name.c_str(), addr_port.c_str());
}

void PaxosWorker::WaitForShutdown() {
  if (hb_rpc_server_ != nullptr) {
    scsi_->server_shutdown();
    delete hb_rpc_server_;
    delete scsi_;
    svr_hb_poll_mgr_g->release();
    hb_thread_pool_g->release();

    for (auto service : services_) {
      if (DepTranServiceImpl* s = dynamic_cast<DepTranServiceImpl*>(service)) {
        auto& recorder = s->recorder_;
        if (recorder) {
          auto n_flush_avg_ = recorder->stat_cnt_.peek().avg_;
          auto sz_flush_avg_ = recorder->stat_sz_.peek().avg_;
          Log::info("Log to disk, average log per flush: %lld,"
                    " average size per flush: %lld",
                    n_flush_avg_, sz_flush_avg_);
        }
      }
    }
  }
}

void PaxosWorker::ShutDown() {
  Log_debug("site %s deleting services, num: %d", site_info_->name.c_str(), services_.size());
  delete rpc_server_;
  for (auto service : services_) {
    delete service;
  }
  thread_pool_g->release();
  int accept_tot_sec_ = 0, accept_tot_usec_ = 0;
  for (auto c : created_coordinators_) {
    accept_tot_sec_ += c->accept_sec_;
    accept_tot_usec_ += c->accept_usec_;
    delete c;
  }
  Log_info("site %s, tot time: %f, prepare: %f, accept: %f, commit: %f", site_info_->name.c_str(),
           submit_tot_sec_ + ((float)submit_tot_usec_) / 1000000,
           prepare_tot_sec_ + ((float)prepare_tot_usec_) / 1000000,
           accept_tot_sec_ + ((float)accept_tot_usec_) / 1000000,
           commit_tot_sec_ + ((float)commit_tot_usec_) / 1000000);
  if (rep_sched_ != nullptr) {
    delete rep_sched_;
  }
}

void PaxosWorker::WaitForSubmit() {
  finish_mutex.lock();
  while (n_current > 0) {
    Log_debug("wait for task, amount: %d", n_current);
    finish_cond.wait(finish_mutex);
  }
  finish_mutex.unlock();
  Log_debug("finish task.");
}

void PaxosWorker::FreeCoordinator(Coordinator* coo) {
  Log_debug("%s: coo_id %d", __FUNCTION__, coo->coo_id_);
  std::lock_guard<std::mutex> lock(coordinator_mutex);
  free_coordinators_.push_back(coo);
}

Coordinator* PaxosWorker::FindOrCreateCoordinator() {
  std::lock_guard<std::mutex> lock(coordinator_mutex);
  static cooid_t cid = 0;
  static id_t id = 0;
  Coordinator* coo = nullptr;

  if (free_coordinators_.size() > 0) {
    coo = dynamic_cast<Coordinator*>(free_coordinators_.back());
    free_coordinators_.pop_back();
  } else {
    if (created_coordinators_.size() == UINT16_MAX) {
      return nullptr;
    }
    verify(created_coordinators_.size() <= UINT16_MAX);
    verify(rep_frame_ != nullptr);
    coo = rep_frame_->CreateCoordinator(cid++,
                                        Config::GetConfig(),
                                        0,
                                        nullptr,
                                        id++,
                                        nullptr);
    coo->par_id_ = site_info_->partition_id_;
    coo->loc_id_ = site_info_->locale_id;
    created_coordinators_.push_back(coo);
  }

  return coo;
}

void PaxosWorker::Submit(const char* log_entry, int length) {
  if (!IsLeader()) return;
  struct timeval t1, t2;
  gettimeofday(&t1, NULL);
  auto sp_cmd = make_shared<LogEntry>();
  sp_cmd->operation_ = new char[length];
  strcpy(sp_cmd->operation_, log_entry);
  sp_cmd->length = length;
  auto sp_m = dynamic_pointer_cast<Marshallable>(sp_cmd);
  _Submit(sp_m);
  gettimeofday(&t2, NULL);
  prepare_tot_sec_ += t2.tv_sec - t1.tv_sec;
  prepare_tot_usec_ += t2.tv_usec - t2.tv_usec;
}

void PaxosWorker::_Submit(shared_ptr<Marshallable> sp_m) {
  finish_mutex.lock();
  n_current++;
  finish_mutex.unlock();
  auto coord = FindOrCreateCoordinator();
  coord->Submit_(sp_m, std::bind(&PaxosWorker::FreeCoordinator,
                                 this,
                                 std::placeholders::_1));

  // leader_commit_time_ = coord->commit_time_;
  // commit_tot_sec_ += commit_time_.tv_sec - leader_commit_time_.tv_sec;
  // commit_tot_usec_ += commit_time_.tv_usec - leader_commit_time_.tv_usec;
}

void PaxosWorker::SubmitExample() {
  char testdata[] = "abc";
  Submit(testdata, 4);
}

bool PaxosWorker::IsLeader() {
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);
  return rep_frame_->site_info_->locale_id == 0;
}

void PaxosWorker::register_apply_callback(std::function<void(char*, int)> cb) {
  this->callback_ = cb;
  verify(rep_sched_ != nullptr);
  rep_sched_->RegLearnerAction(std::bind(&PaxosWorker::Next,
                                         this,
                                         std::placeholders::_1));
}

} // namespace janus