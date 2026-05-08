// ssh2ng — N-API binding to libssh.
//
// Architecture:
//   ssh2ng.Client (JS) → SshSession (C++) → ssh_session_struct (libssh)
//
// Each Client owns one ssh_session. Operations (connect/exec/sftp) run
// on the libuv thread pool via AsyncWorker so they don't block the JS
// event loop.
//
// libssh sessions are NOT thread-safe — only one operation may use the
// session at a time. The JS layer serializes calls.

#include <napi.h>

extern "C" {
#include <libssh/libssh.h>
#include <libssh/sftp.h>
}

#include <fcntl.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- Session ----------------------------------------------------------------

class SshSession : public Napi::ObjectWrap<SshSession> {
public:
  static Napi::Function GetClass(Napi::Env env);

  SshSession(const Napi::CallbackInfo& info);
  ~SshSession();

  ssh_session session_ = nullptr;
  sftp_session sftp_ = nullptr;

private:
  Napi::Value Connect(const Napi::CallbackInfo& info);
  Napi::Value Exec(const Napi::CallbackInfo& info);
  Napi::Value ExecStream(const Napi::CallbackInfo& info);
  Napi::Value SftpInit(const Napi::CallbackInfo& info);
  Napi::Value SftpReadFile(const Napi::CallbackInfo& info);
  Napi::Value SftpWriteFile(const Napi::CallbackInfo& info);
  Napi::Value SftpStat(const Napi::CallbackInfo& info);
  Napi::Value SftpRename(const Napi::CallbackInfo& info);
  Napi::Value SftpUnlink(const Napi::CallbackInfo& info);
  Napi::Value SftpMkdir(const Napi::CallbackInfo& info);
  Napi::Value End(const Napi::CallbackInfo& info);
};

// ---- ConnectWorker ----------------------------------------------------------

class ConnectWorker : public Napi::AsyncWorker {
public:
  ConnectWorker(Napi::Env env, SshSession* sess,
                std::string host, std::string user, int port,
                int timeoutSec, bool acceptAnyHostKey)
    : Napi::AsyncWorker(env),
      deferred_(Napi::Promise::Deferred::New(env)),
      sess_(sess), host_(std::move(host)), user_(std::move(user)),
      port_(port), timeout_(timeoutSec), acceptAnyHostKey_(acceptAnyHostKey) {}

  Napi::Promise GetPromise() { return deferred_.Promise(); }

  void Execute() override {
    ssh_session s = sess_->session_;

    if (ssh_options_set(s, SSH_OPTIONS_HOST, host_.c_str()) != SSH_OK) {
      err_ = std::string("set host failed: ") + ssh_get_error(s);
      return;
    }
    if (timeout_ > 0) {
      long t = static_cast<long>(timeout_);
      if (ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &t) != SSH_OK) {
        err_ = std::string("set timeout failed: ") + ssh_get_error(s);
        return;
      }
    }

    // KEY MOMENT — make libssh consult ~/.ssh/config. This pulls in
    // HostName, User, Port, IdentityFile, ProxyJump, ProxyCommand,
    // CertificateFile, and every other host-specific stanza.
    if (ssh_options_parse_config(s, nullptr) != SSH_OK) {
      err_ = std::string("parse_config failed: ") + ssh_get_error(s);
      return;
    }

    if (!user_.empty()) {
      if (ssh_options_set(s, SSH_OPTIONS_USER, user_.c_str()) != SSH_OK) {
        err_ = std::string("set user failed: ") + ssh_get_error(s);
        return;
      }
    }
    if (port_ > 0) {
      unsigned int p = static_cast<unsigned int>(port_);
      if (ssh_options_set(s, SSH_OPTIONS_PORT, &p) != SSH_OK) {
        err_ = std::string("set port failed: ") + ssh_get_error(s);
        return;
      }
    }

    if (ssh_connect(s) != SSH_OK) {
      err_ = std::string("connect failed: ") + ssh_get_error(s);
      return;
    }

    if (!acceptAnyHostKey_) {
      enum ssh_known_hosts_e known = ssh_session_is_known_server(s);
      if (known != SSH_KNOWN_HOSTS_OK) {
        ssh_disconnect(s);
        err_ = "host key not in known_hosts (set strictHostKeyChecking=false to accept)";
        return;
      }
    }

    // Auth: try agent + default keys via publickey_auto. libssh internally
    // walks IdentityFile entries from the parsed config + agent.
    int rc = ssh_userauth_publickey_auto(s, nullptr, nullptr);
    if (rc != SSH_AUTH_SUCCESS) {
      err_ = std::string("authentication failed: ") + ssh_get_error(s);
      ssh_disconnect(s);
      return;
    }
  }

  void OnOK() override {
    if (!err_.empty()) {
      deferred_.Reject(Napi::Error::New(Env(), err_).Value());
    } else {
      deferred_.Resolve(Env().Undefined());
    }
  }

  void OnError(const Napi::Error& e) override {
    deferred_.Reject(e.Value());
  }

private:
  Napi::Promise::Deferred deferred_;
  SshSession* sess_;
  std::string host_, user_;
  int port_, timeout_;
  bool acceptAnyHostKey_;
  std::string err_;
};

// ---- ExecWorker -------------------------------------------------------------

struct ExecResult {
  std::vector<uint8_t> stdout_buf;
  std::vector<uint8_t> stderr_buf;
  int exit_status = -1;
  std::string error;
};

class ExecWorker : public Napi::AsyncWorker {
public:
  ExecWorker(Napi::Env env, SshSession* sess, std::string command,
             std::vector<uint8_t> stdin_data, bool has_stdin)
    : Napi::AsyncWorker(env),
      deferred_(Napi::Promise::Deferred::New(env)),
      sess_(sess), command_(std::move(command)),
      stdin_data_(std::move(stdin_data)), has_stdin_(has_stdin) {}

  Napi::Promise GetPromise() { return deferred_.Promise(); }

  void Execute() override {
    ssh_session s = sess_->session_;
    ssh_channel ch = ssh_channel_new(s);
    if (!ch) { result_.error = "channel_new failed"; return; }

    if (ssh_channel_open_session(ch) != SSH_OK) {
      result_.error = std::string("channel_open_session: ") + ssh_get_error(s);
      ssh_channel_free(ch);
      return;
    }

    if (ssh_channel_request_exec(ch, command_.c_str()) != SSH_OK) {
      result_.error = std::string("channel_request_exec: ") + ssh_get_error(s);
      ssh_channel_close(ch);
      ssh_channel_free(ch);
      return;
    }

    if (has_stdin_ && !stdin_data_.empty()) {
      size_t total = 0;
      while (total < stdin_data_.size()) {
        int wrote = ssh_channel_write(ch, stdin_data_.data() + total,
                                      static_cast<uint32_t>(stdin_data_.size() - total));
        if (wrote == SSH_ERROR) {
          result_.error = std::string("channel_write: ") + ssh_get_error(s);
          ssh_channel_close(ch);
          ssh_channel_free(ch);
          return;
        }
        total += static_cast<size_t>(wrote);
      }
    }
    if (has_stdin_) ssh_channel_send_eof(ch);

    char buf[16384];
    while (!ssh_channel_is_eof(ch)) {
      int n = ssh_channel_read(ch, buf, sizeof(buf), 0);
      if (n > 0) result_.stdout_buf.insert(result_.stdout_buf.end(), buf, buf + n);
      else if (n == SSH_ERROR) { result_.error = ssh_get_error(s); break; }
      else if (n == 0) break;

      n = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), 1);
      if (n > 0) result_.stderr_buf.insert(result_.stderr_buf.end(), buf, buf + n);
    }
    // Drain stderr after stdout EOF.
    int n;
    while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) {
      result_.stderr_buf.insert(result_.stderr_buf.end(), buf, buf + n);
    }

    result_.exit_status = ssh_channel_get_exit_status(ch);
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
  }

  void OnOK() override {
    Napi::Env env = Env();
    if (!result_.error.empty()) {
      deferred_.Reject(Napi::Error::New(env, result_.error).Value());
      return;
    }
    Napi::Object out = Napi::Object::New(env);
    out.Set("stdout", Napi::Buffer<uint8_t>::Copy(env, result_.stdout_buf.data(), result_.stdout_buf.size()));
    out.Set("stderr", Napi::Buffer<uint8_t>::Copy(env, result_.stderr_buf.data(), result_.stderr_buf.size()));
    out.Set("code", Napi::Number::New(env, result_.exit_status));
    deferred_.Resolve(out);
  }

  void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
  Napi::Promise::Deferred deferred_;
  SshSession* sess_;
  std::string command_;
  std::vector<uint8_t> stdin_data_;
  bool has_stdin_;
  ExecResult result_;
};

// ---- SFTP workers -----------------------------------------------------------

class SftpInitWorker : public Napi::AsyncWorker {
public:
  SftpInitWorker(Napi::Env env, SshSession* sess)
    : Napi::AsyncWorker(env),
      deferred_(Napi::Promise::Deferred::New(env)), sess_(sess) {}
  Napi::Promise GetPromise() { return deferred_.Promise(); }
  void Execute() override {
    if (sess_->sftp_) return;
    sftp_session sftp = sftp_new(sess_->session_);
    if (!sftp) { err_ = std::string("sftp_new: ") + ssh_get_error(sess_->session_); return; }
    if (sftp_init(sftp) != SSH_OK) { err_ = std::string("sftp_init: ") + ssh_get_error(sess_->session_); sftp_free(sftp); return; }
    sess_->sftp_ = sftp;
  }
  void OnOK() override {
    if (!err_.empty()) deferred_.Reject(Napi::Error::New(Env(), err_).Value());
    else deferred_.Resolve(Env().Undefined());
  }
  void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
private:
  Napi::Promise::Deferred deferred_;
  SshSession* sess_;
  std::string err_;
};

class SftpReadFileWorker : public Napi::AsyncWorker {
public:
  SftpReadFileWorker(Napi::Env env, SshSession* sess, std::string path)
    : Napi::AsyncWorker(env),
      deferred_(Napi::Promise::Deferred::New(env)),
      sess_(sess), path_(std::move(path)) {}
  Napi::Promise GetPromise() { return deferred_.Promise(); }
  void Execute() override {
    sftp_session sftp = sess_->sftp_;
    if (!sftp) { err_ = "sftp not initialized"; return; }
    sftp_file f = sftp_open(sftp, path_.c_str(), O_RDONLY, 0);
    if (!f) { err_ = std::string("sftp_open: ") + ssh_get_error(sess_->session_); return; }
    char buf[32 * 1024];
    while (true) {
      ssize_t n = sftp_read(f, buf, sizeof(buf));
      if (n < 0) { err_ = std::string("sftp_read: ") + ssh_get_error(sess_->session_); break; }
      if (n == 0) break;
      data_.insert(data_.end(), buf, buf + n);
    }
    sftp_close(f);
  }
  void OnOK() override {
    Napi::Env env = Env();
    if (!err_.empty()) deferred_.Reject(Napi::Error::New(env, err_).Value());
    else deferred_.Resolve(Napi::Buffer<uint8_t>::Copy(env, data_.data(), data_.size()));
  }
  void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
private:
  Napi::Promise::Deferred deferred_;
  SshSession* sess_;
  std::string path_;
  std::vector<uint8_t> data_;
  std::string err_;
};

class SftpWriteFileWorker : public Napi::AsyncWorker {
public:
  SftpWriteFileWorker(Napi::Env env, SshSession* sess,
                      std::string path, std::vector<uint8_t> data, int mode)
    : Napi::AsyncWorker(env),
      deferred_(Napi::Promise::Deferred::New(env)),
      sess_(sess), path_(std::move(path)),
      data_(std::move(data)), mode_(mode) {}
  Napi::Promise GetPromise() { return deferred_.Promise(); }
  void Execute() override {
    sftp_session sftp = sess_->sftp_;
    if (!sftp) { err_ = "sftp not initialized"; return; }
    sftp_file f = sftp_open(sftp, path_.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC, mode_);
    if (!f) { err_ = std::string("sftp_open: ") + ssh_get_error(sess_->session_); return; }
    size_t total = 0;
    while (total < data_.size()) {
      ssize_t n = sftp_write(f, data_.data() + total, data_.size() - total);
      if (n < 0) { err_ = std::string("sftp_write: ") + ssh_get_error(sess_->session_); break; }
      total += static_cast<size_t>(n);
    }
    sftp_close(f);
  }
  void OnOK() override {
    if (!err_.empty()) deferred_.Reject(Napi::Error::New(Env(), err_).Value());
    else deferred_.Resolve(Env().Undefined());
  }
  void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
private:
  Napi::Promise::Deferred deferred_;
  SshSession* sess_;
  std::string path_;
  std::vector<uint8_t> data_;
  int mode_;
  std::string err_;
};

class SftpSimpleWorker : public Napi::AsyncWorker {
public:
  enum Op { STAT, RENAME, UNLINK, MKDIR };

  SftpSimpleWorker(Napi::Env env, SshSession* sess, Op op,
                   std::string p1, std::string p2 = "", int mode = 0755)
    : Napi::AsyncWorker(env),
      deferred_(Napi::Promise::Deferred::New(env)),
      sess_(sess), op_(op), p1_(std::move(p1)), p2_(std::move(p2)), mode_(mode) {}
  Napi::Promise GetPromise() { return deferred_.Promise(); }
  void Execute() override {
    sftp_session sftp = sess_->sftp_;
    if (!sftp) { err_ = "sftp not initialized"; return; }
    switch (op_) {
      case STAT: {
        sftp_attributes a = sftp_stat(sftp, p1_.c_str());
        if (!a) { err_ = std::string("sftp_stat: ") + ssh_get_error(sess_->session_); return; }
        size_ = a->size;
        mtime_ = a->mtime;
        is_dir_ = (a->type == SSH_FILEXFER_TYPE_DIRECTORY);
        is_reg_ = (a->type == SSH_FILEXFER_TYPE_REGULAR);
        sftp_attributes_free(a);
        break;
      }
      case RENAME:
        if (sftp_rename(sftp, p1_.c_str(), p2_.c_str()) != SSH_OK)
          err_ = std::string("sftp_rename: ") + ssh_get_error(sess_->session_);
        break;
      case UNLINK:
        if (sftp_unlink(sftp, p1_.c_str()) != SSH_OK)
          err_ = std::string("sftp_unlink: ") + ssh_get_error(sess_->session_);
        break;
      case MKDIR:
        if (sftp_mkdir(sftp, p1_.c_str(), mode_) != SSH_OK) {
          // EEXIST is acceptable for mkdir -p semantics; surface other errors.
          int code = sftp_get_error(sftp);
          if (code != SSH_FX_FILE_ALREADY_EXISTS)
            err_ = std::string("sftp_mkdir: ") + ssh_get_error(sess_->session_);
        }
        break;
    }
  }
  void OnOK() override {
    Napi::Env env = Env();
    if (!err_.empty()) { deferred_.Reject(Napi::Error::New(env, err_).Value()); return; }
    if (op_ == STAT) {
      Napi::Object o = Napi::Object::New(env);
      o.Set("size", Napi::Number::New(env, static_cast<double>(size_)));
      o.Set("mtime", Napi::Number::New(env, static_cast<double>(mtime_)));
      o.Set("isDirectory", Napi::Boolean::New(env, is_dir_));
      o.Set("isFile", Napi::Boolean::New(env, is_reg_));
      deferred_.Resolve(o);
    } else {
      deferred_.Resolve(env.Undefined());
    }
  }
  void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
private:
  Napi::Promise::Deferred deferred_;
  SshSession* sess_;
  Op op_;
  std::string p1_, p2_;
  int mode_;
  std::string err_;
  uint64_t size_ = 0;
  uint64_t mtime_ = 0;
  bool is_dir_ = false, is_reg_ = false;
};

// ---- SshExecChannel ---------------------------------------------------------
//
// Streaming exec channel: spawns a worker thread that drives the channel
// reads, pushes stdout/stderr chunks into JS via ThreadSafeFunctions, and
// reports the final exit code. Honors abort() from JS at the next 100ms
// poll boundary by sending SIGTERM through the SSH signal channel and
// closing the channel.
//
// Lifecycle:
//   const ch = new SshExecChannel(session, command, stdin?)
//   ch.start(onData, onStderr, onClose)
//   ch.abort()             // optional
// JS holds the only strong reference; when GC'd, the destructor signals
// the worker, joins it, and releases the TSFNs.

class SshExecChannel : public Napi::ObjectWrap<SshExecChannel> {
public:
  static Napi::Function GetClass(Napi::Env env);
  SshExecChannel(const Napi::CallbackInfo& info);
  ~SshExecChannel();

private:
  Napi::Value Start(const Napi::CallbackInfo& info);
  Napi::Value Abort(const Napi::CallbackInfo& info);
  Napi::Value WriteStdin(const Napi::CallbackInfo& info);
  Napi::Value EndStdin(const Napi::CallbackInfo& info);

  void Run();

  // Owning reference. Stored as a Napi::ObjectReference so the parent
  // session JS object can't be GC'd while the worker thread is running.
  Napi::ObjectReference session_ref_;
  SshSession* sess_ = nullptr;
  std::string command_;
  std::vector<uint8_t> initial_stdin_;
  bool has_initial_stdin_ = false;

  std::thread thread_;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> stdin_closed_{false};
  std::atomic<bool> running_{false};

  // Pending stdin writes (from JS). Worker drains and forwards to remote.
  std::mutex stdin_mu_;
  std::vector<uint8_t> stdin_pending_;

  Napi::ThreadSafeFunction tsfn_data_;
  Napi::ThreadSafeFunction tsfn_stderr_;
  Napi::ThreadSafeFunction tsfn_close_;
};

Napi::Function SshExecChannel::GetClass(Napi::Env env) {
  return DefineClass(env, "SshExecChannel", {
    InstanceMethod("start", &SshExecChannel::Start),
    InstanceMethod("abort", &SshExecChannel::Abort),
    InstanceMethod("writeStdin", &SshExecChannel::WriteStdin),
    InstanceMethod("endStdin", &SshExecChannel::EndStdin),
  });
}

SshExecChannel::SshExecChannel(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<SshExecChannel>(info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsString()) {
    Napi::Error::New(env, "SshExecChannel(session, command, stdin?)").ThrowAsJavaScriptException();
    return;
  }
  Napi::Object sess_obj = info[0].As<Napi::Object>();
  sess_ = Napi::ObjectWrap<SshSession>::Unwrap(sess_obj);
  session_ref_ = Napi::Persistent(sess_obj);
  command_ = info[1].As<Napi::String>().Utf8Value();
  if (info.Length() > 2 && info[2].IsBuffer()) {
    auto buf = info[2].As<Napi::Buffer<uint8_t>>();
    initial_stdin_.assign(buf.Data(), buf.Data() + buf.Length());
    has_initial_stdin_ = true;
  }
}

SshExecChannel::~SshExecChannel() {
  aborted_ = true;
  stdin_closed_ = true;
  if (thread_.joinable()) thread_.join();
  // TSFNs are released inside Run() when the worker finishes; if start
  // was never called they're empty and Release is a no-op.
}

Napi::Value SshExecChannel::Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (running_.exchange(true)) {
    Napi::Error::New(env, "SshExecChannel already started").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 3 || !info[0].IsFunction() || !info[1].IsFunction() || !info[2].IsFunction()) {
    Napi::Error::New(env, "start(onData, onStderr, onClose)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  tsfn_data_ = Napi::ThreadSafeFunction::New(env, info[0].As<Napi::Function>(), "ssh2ng_exec_data", 0, 1);
  tsfn_stderr_ = Napi::ThreadSafeFunction::New(env, info[1].As<Napi::Function>(), "ssh2ng_exec_stderr", 0, 1);
  tsfn_close_ = Napi::ThreadSafeFunction::New(env, info[2].As<Napi::Function>(), "ssh2ng_exec_close", 0, 1);

  thread_ = std::thread([this]() { this->Run(); });
  return env.Undefined();
}

Napi::Value SshExecChannel::Abort(const Napi::CallbackInfo& info) {
  aborted_ = true;
  return info.Env().Undefined();
}

Napi::Value SshExecChannel::WriteStdin(const Napi::CallbackInfo& info) {
  if (!info[0].IsBuffer()) {
    Napi::Error::New(info.Env(), "writeStdin(buffer)").ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  auto buf = info[0].As<Napi::Buffer<uint8_t>>();
  std::lock_guard<std::mutex> lock(stdin_mu_);
  stdin_pending_.insert(stdin_pending_.end(), buf.Data(), buf.Data() + buf.Length());
  return info.Env().Undefined();
}

Napi::Value SshExecChannel::EndStdin(const Napi::CallbackInfo& info) {
  stdin_closed_ = true;
  return info.Env().Undefined();
}

namespace {
// Helper: forward a buffer slice to a JS callback via a TSFN.
void post_chunk(Napi::ThreadSafeFunction& tsfn, const uint8_t* data, size_t len) {
  if (len == 0) return;
  // Copy into a heap vector; freed in the JS-thread callback.
  auto* owned = new std::vector<uint8_t>(data, data + len);
  napi_status st = tsfn.NonBlockingCall(owned, [](Napi::Env env, Napi::Function jsCb, std::vector<uint8_t>* d) {
    if (!d) return;
    auto buf = Napi::Buffer<uint8_t>::Copy(env, d->data(), d->size());
    delete d;
    jsCb.Call({ buf });
  });
  if (st != napi_ok) delete owned;
}
} // anon

void SshExecChannel::Run() {
  ssh_session s = sess_->session_;
  ssh_channel ch = ssh_channel_new(s);

  int exit_status = -1;
  std::string fail;

  if (!ch) {
    fail = "channel_new failed";
  } else if (ssh_channel_open_session(ch) != SSH_OK) {
    fail = std::string("channel_open_session: ") + ssh_get_error(s);
    ssh_channel_free(ch);
    ch = nullptr;
  } else if (ssh_channel_request_exec(ch, command_.c_str()) != SSH_OK) {
    fail = std::string("channel_request_exec: ") + ssh_get_error(s);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
    ch = nullptr;
  }

  if (ch) {
    // Send any stdin queued before start().
    if (has_initial_stdin_ && !initial_stdin_.empty()) {
      size_t total = 0;
      while (total < initial_stdin_.size()) {
        int wrote = ssh_channel_write(ch, initial_stdin_.data() + total,
                                      static_cast<uint32_t>(initial_stdin_.size() - total));
        if (wrote == SSH_ERROR) { fail = ssh_get_error(s); break; }
        total += static_cast<size_t>(wrote);
      }
    }

    char buf[16384];

    while (!aborted_.load() && !ssh_channel_is_eof(ch)) {
      // Drain any stdin queued from JS while we run.
      {
        std::vector<uint8_t> pending;
        {
          std::lock_guard<std::mutex> lock(stdin_mu_);
          pending.swap(stdin_pending_);
        }
        if (!pending.empty()) {
          size_t total = 0;
          while (total < pending.size()) {
            int wrote = ssh_channel_write(ch, pending.data() + total,
                                          static_cast<uint32_t>(pending.size() - total));
            if (wrote == SSH_ERROR) { fail = ssh_get_error(s); break; }
            total += static_cast<size_t>(wrote);
          }
        }
      }
      if (stdin_closed_.load()) {
        ssh_channel_send_eof(ch);
        // After EOF we don't have more to write; flag stays set.
        stdin_closed_ = true;
      }

      // Wait up to 100ms for stdout. Short timeout keeps us responsive
      // to abort() and to incoming stdin writes.
      int n = ssh_channel_read_timeout(ch, buf, sizeof(buf), 0, 100);
      if (n > 0) post_chunk(tsfn_data_, reinterpret_cast<uint8_t*>(buf), n);
      else if (n == SSH_ERROR) { fail = ssh_get_error(s); break; }
      // n == 0 → EOF on stdout; loop will check is_eof().
      // n == SSH_AGAIN → no data this tick; fall through.

      // Drain stderr non-blockingly so it interleaves naturally.
      int m = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), 1);
      if (m > 0) post_chunk(tsfn_stderr_, reinterpret_cast<uint8_t*>(buf), m);
      else if (m == SSH_ERROR && fail.empty()) fail = ssh_get_error(s);
    }

    // Final stderr drain.
    int m;
    while ((m = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), 1)) > 0) {
      post_chunk(tsfn_stderr_, reinterpret_cast<uint8_t*>(buf), m);
    }

    if (aborted_.load()) {
      // Best-effort SIGTERM to the remote process.
      ssh_channel_request_send_signal(ch, "TERM");
    }

    exit_status = ssh_channel_get_exit_status(ch);
    if (!stdin_closed_.load()) ssh_channel_send_eof(ch);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
  }

  // Tell JS we're done.
  struct CloseArgs { int code; bool aborted; std::string error; };
  auto* args = new CloseArgs{ exit_status, aborted_.load(), std::move(fail) };
  napi_status st = tsfn_close_.NonBlockingCall(args, [](Napi::Env env, Napi::Function jsCb, CloseArgs* a) {
    Napi::Object o = Napi::Object::New(env);
    o.Set("code", Napi::Number::New(env, a->code));
    o.Set("aborted", Napi::Boolean::New(env, a->aborted));
    if (!a->error.empty()) o.Set("error", Napi::String::New(env, a->error));
    delete a;
    jsCb.Call({ o });
  });
  if (st != napi_ok) delete args;

  tsfn_data_.Release();
  tsfn_stderr_.Release();
  tsfn_close_.Release();
}

// ---- SshSession bindings ----------------------------------------------------

Napi::Function SshSession::GetClass(Napi::Env env) {
  return DefineClass(env, "SshSession", {
    InstanceMethod("connect", &SshSession::Connect),
    InstanceMethod("exec", &SshSession::Exec),
    InstanceMethod("execStream", &SshSession::ExecStream),
    InstanceMethod("sftpInit", &SshSession::SftpInit),
    InstanceMethod("sftpReadFile", &SshSession::SftpReadFile),
    InstanceMethod("sftpWriteFile", &SshSession::SftpWriteFile),
    InstanceMethod("sftpStat", &SshSession::SftpStat),
    InstanceMethod("sftpRename", &SshSession::SftpRename),
    InstanceMethod("sftpUnlink", &SshSession::SftpUnlink),
    InstanceMethod("sftpMkdir", &SshSession::SftpMkdir),
    InstanceMethod("end", &SshSession::End),
  });
}

SshSession::SshSession(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<SshSession>(info) {
  session_ = ssh_new();
  if (!session_) {
    Napi::Error::New(info.Env(), "ssh_new failed (out of memory?)").ThrowAsJavaScriptException();
  }
}

SshSession::~SshSession() {
  if (sftp_) { sftp_free(sftp_); sftp_ = nullptr; }
  if (session_) {
    ssh_disconnect(session_);
    ssh_free(session_);
    session_ = nullptr;
  }
}

Napi::Value SshSession::Connect(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object opts = info[0].As<Napi::Object>();
  std::string host = opts.Get("host").As<Napi::String>().Utf8Value();
  std::string user = opts.Has("username") ? opts.Get("username").As<Napi::String>().Utf8Value() : "";
  int port = opts.Has("port") ? opts.Get("port").As<Napi::Number>().Int32Value() : 0;
  int timeout = opts.Has("timeoutSec") ? opts.Get("timeoutSec").As<Napi::Number>().Int32Value() : 15;
  bool acceptAny = opts.Has("acceptAnyHostKey") ? opts.Get("acceptAnyHostKey").As<Napi::Boolean>().Value() : true;

  auto* w = new ConnectWorker(env, this, host, user, port, timeout, acceptAny);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::Exec(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string cmd = info[0].As<Napi::String>().Utf8Value();
  std::vector<uint8_t> stdin_data;
  bool has_stdin = false;
  if (info.Length() > 1 && info[1].IsBuffer()) {
    auto buf = info[1].As<Napi::Buffer<uint8_t>>();
    stdin_data.assign(buf.Data(), buf.Data() + buf.Length());
    has_stdin = true;
  }
  auto* w = new ExecWorker(env, this, cmd, std::move(stdin_data), has_stdin);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

// Streaming variant. Constructs an `SshExecChannel` JS instance bound to
// this session — the caller then registers stdout/stderr/close TSFNs via
// `.start()`. See SshExecChannel for the full lifecycle.
Napi::Value SshSession::ExecStream(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Function ctor = SshExecChannel::GetClass(env);
  std::vector<napi_value> args;
  args.push_back(this->Value());           // session
  args.push_back(info[0]);                 // command
  if (info.Length() > 1) args.push_back(info[1]); // stdin (optional)
  return ctor.New(args);
}

Napi::Value SshSession::SftpInit(const Napi::CallbackInfo& info) {
  auto* w = new SftpInitWorker(info.Env(), this);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::SftpReadFile(const Napi::CallbackInfo& info) {
  std::string path = info[0].As<Napi::String>().Utf8Value();
  auto* w = new SftpReadFileWorker(info.Env(), this, path);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::SftpWriteFile(const Napi::CallbackInfo& info) {
  std::string path = info[0].As<Napi::String>().Utf8Value();
  auto buf = info[1].As<Napi::Buffer<uint8_t>>();
  std::vector<uint8_t> data(buf.Data(), buf.Data() + buf.Length());
  int mode = info.Length() > 2 ? info[2].As<Napi::Number>().Int32Value() : 0644;
  auto* w = new SftpWriteFileWorker(info.Env(), this, path, std::move(data), mode);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::SftpStat(const Napi::CallbackInfo& info) {
  std::string path = info[0].As<Napi::String>().Utf8Value();
  auto* w = new SftpSimpleWorker(info.Env(), this, SftpSimpleWorker::STAT, path);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::SftpRename(const Napi::CallbackInfo& info) {
  std::string oldp = info[0].As<Napi::String>().Utf8Value();
  std::string newp = info[1].As<Napi::String>().Utf8Value();
  auto* w = new SftpSimpleWorker(info.Env(), this, SftpSimpleWorker::RENAME, oldp, newp);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::SftpUnlink(const Napi::CallbackInfo& info) {
  std::string path = info[0].As<Napi::String>().Utf8Value();
  auto* w = new SftpSimpleWorker(info.Env(), this, SftpSimpleWorker::UNLINK, path);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::SftpMkdir(const Napi::CallbackInfo& info) {
  std::string path = info[0].As<Napi::String>().Utf8Value();
  int mode = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : 0755;
  auto* w = new SftpSimpleWorker(info.Env(), this, SftpSimpleWorker::MKDIR, path, "", mode);
  Napi::Promise p = w->GetPromise();
  w->Queue();
  return p;
}

Napi::Value SshSession::End(const Napi::CallbackInfo& info) {
  if (sftp_) { sftp_free(sftp_); sftp_ = nullptr; }
  if (session_) {
    ssh_disconnect(session_);
    ssh_free(session_);
    session_ = nullptr;
  }
  return info.Env().Undefined();
}

// ---- Module init ------------------------------------------------------------

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("SshSession", SshSession::GetClass(env));
  exports.Set("SshExecChannel", SshExecChannel::GetClass(env));
  return exports;
}

} // namespace

NODE_API_MODULE(ssh2ng, Init)
