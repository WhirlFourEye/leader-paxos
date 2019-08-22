#pragma once

#include "__dep__.h"
#include "coordinator.h"
#include "benchmark_control_rpc.h"
#include "frame.h"
#include "scheduler.h"
#include "communicator.h"
#include "config.h"

namespace janus {

class LogEntry : public Marshallable {
public:
  char* operation_ = nullptr;

  LogEntry() : Marshallable(MarshallDeputy::CONTAINER_CMD) {}
  virtual ~LogEntry(){
    if (operation_ != nullptr) delete operation_;
  }
  virtual Marshal& ToMarshal(Marshal&) const override;
  virtual Marshal& FromMarshal(Marshal&) override;
};

class PaxosWorker {
private:
  void _Submit(shared_ptr<Marshallable>);
  bool IsLeader();

  rrr::Mutex finish_mutex{};
  rrr::CondVar finish_cond{};
  uint32_t n_current = 0;
  std::function<void(char*)> callback_ = nullptr;
  vector<Coordinator*> created_coordinators_{};

public:
  rrr::PollMgr* svr_poll_mgr_ = nullptr;
  vector<rrr::Service*> services_ = {};
  rrr::Server* rpc_server_ = nullptr;
  base::ThreadPool* thread_pool_g = nullptr;

  rrr::PollMgr* svr_hb_poll_mgr_g = nullptr;
  ServerControlServiceImpl* scsi_ = nullptr;
  rrr::Server* hb_rpc_server_ = nullptr;
  base::ThreadPool* hb_thread_pool_g = nullptr;

  Config::SiteInfo* site_info_ = nullptr;
  Frame* rep_frame_ = nullptr;
  Scheduler* rep_sched_ = nullptr;
  Communicator* rep_commo_ = nullptr;

  void SetupHeartbeat();
  void SetupBase();
  void SetupService();
  void SetupCommo();
  void ShutDown();
  void Next(Marshallable&);

  static const uint32_t CtrlPortDelta = 10000;
  void WaitForShutdown();

  void SubmitExample();
  void Submit(const char*);
  void register_apply_callback(std::function<void(char*)>);
};

} // namespace janus
