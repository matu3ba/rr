/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <limits.h>
#include <memory>
#include <sstream>

#include "launch_debugger.h"

#include "GdbCommandHandler.h"
#include "GdbServer.h"
#include "GdbServerConnection.h"
#include "log.h"
#include "StringVectorToCharArray.h"
#include "util.h"

using namespace std;

namespace rr {

// Special-sauce macros defined by rr when launching the gdb client,
// which implement functionality outside of the gdb remote protocol.
// (Don't stare at them too long or you'll go blind ;).)
static const string& gdb_rr_macros() {
  static string s;

  if (s.empty()) {
    stringstream ss;
    ss << GdbCommandHandler::gdb_macros()
       << "define restart\n"
       << "  run c$arg0\n"
       << "end\n"
       << "document restart\n"
       << "restart at checkpoint N\n"
       << "checkpoints are created with the 'checkpoint' command\n"
       << "end\n"
       << "define seek-ticks\n"
       << "  run t$arg0\n"
       << "end\n"
       << "document seek-ticks\n"
       << "restart at given ticks value\n"
       << "end\n"
       << "define jump\n"
       << "  rr-denied jump\n"
       << "end\n"
       // In gdb version "Fedora 7.8.1-30.fc21", a raw "run" command
       // issued before any user-generated resume-execution command
       // results in gdb hanging just after the inferior hits an internal
       // gdb breakpoint.  This happens outside of rr, with gdb
       // controlling gdbserver, as well.  We work around that by
       // ensuring *some* resume-execution command has been issued before
       // restarting the session.  But, only if the inferior hasn't
       // already finished execution ($_thread != 0).  If it has and we
       // issue the "stepi" command, then gdb refuses to restart
       // execution.
       << "define hook-run\n"
       << "  rr-hook-run\n"
       << "end\n"
       << "define hookpost-continue\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-step\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-stepi\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-next\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-nexti\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-finish\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-reverse-continue\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-reverse-step\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-reverse-stepi\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-reverse-finish\n"
       << "  rr-set-suppress-run-hook 1\n"
       << "end\n"
       << "define hookpost-run\n"
       << "  rr-set-suppress-run-hook 0\n"
       << "end\n"
       << "set unwindonsignal on\n"
       << "handle SIGURG stop\n"
       << "set prompt (rr) \n"
       // Try both "set target-async" and "maint set target-async" since
       // that changed recently.
       << "python\n"
       << "import re\n"
       << "m = re.compile(r"
       << "'[^0-9]*([0-9]+)\\.([0-9]+)(\\.([0-9]+))?'"
       << ").match(gdb.VERSION)\n"
       << "ver = int(m.group(1))*10000 + int(m.group(2))*100\n"
       << "if m.group(4):\n"
       << "    ver = ver + int(m.group(4))\n"
       << "\n"
       << "if ver == 71100:\n"
       << "    gdb.write("
       << "'This version of gdb (7.11.0) has known bugs that break rr. "
       << "Install 7.11.1 or later.\\n', gdb.STDERR)\n"
       << "\n"
       << "if ver < 71101:\n"
       << "    gdb.execute('set target-async 0')\n"
       << "    gdb.execute('maint set target-async 0')\n"
       << "end\n";
    s = ss.str();
  }
  return s;
}

struct DebuggerParams {
  char exe_image[PATH_MAX];
  char host[16]; // INET_ADDRSTRLEN, omitted for header churn
  short port;
};

static void push_default_gdb_options(vector<string>& vec, bool serve_files) {
  // The gdb protocol uses the "vRun" packet to reload
  // remote targets.  The packet is specified to be like
  // "vCont", in which gdb waits infinitely long for a
  // stop reply packet.  But in practice, gdb client
  // expects the vRun to complete within the remote-reply
  // timeout, after which it issues vCont.  The timeout
  // causes gdb<-->rr communication to go haywire.
  //
  // rr can take a very long time indeed to send the
  // stop-reply to gdb after restarting replay; the time
  // to reach a specified execution target is
  // theoretically unbounded.  Timing out on vRun is
  // technically a gdb bug, but because the rr replay and
  // the gdb reload models don't quite match up, we'll
  // work around it on the rr side by disabling the
  // remote-reply timeout.
  vec.push_back("-l");
  vec.push_back("10000");
  if (!serve_files) {
    // For now, avoid requesting binary files through vFile. That is slow and
    // hard to make work correctly, because gdb requests files based on the
    // names it sees in memory and in ELF, and those names may be symlinks to
    // the filenames in the trace, so it's hard to match those names to files in
    // the trace.
    vec.push_back("-ex");
    vec.push_back("set sysroot /");
  }
}

static void push_gdb_target_remote_cmd(vector<string>& vec, const string& host,
                                       unsigned short port) {
  vec.push_back("-ex");
  stringstream ss;
  // If we omit the address, then gdb can try to resolve "localhost" which
  // in some broken environments may not actually resolve to the local host
  ss << "target extended-remote " << host << ":" << port;
  vec.push_back(ss.str());
}

string saved_debugger_launch_command;

vector<string> debugger_launch_command(Task* t, const string& host,
                                       unsigned short port,
                                       bool serve_files,
                                       const string& debugger_name) {
  vector<string> cmd;
  cmd.push_back(debugger_name);
  push_default_gdb_options(cmd, serve_files);
  push_gdb_target_remote_cmd(cmd, host, port);
  cmd.push_back(t->vm()->exe_image());
  saved_debugger_launch_command = to_shell_string(cmd);
  return cmd;
}

static string create_gdb_command_file(const string& macros) {
  TempFile file = create_temporary_file("rr-gdb-commands-XXXXXX");
  // This fd is just leaked. That's fine since we only call this once
  // per rr invocation at the moment.
  int fd = file.fd.extract();
  unlink(file.name.c_str());

  ssize_t len = macros.size();
  int written = write(fd, macros.c_str(), len);
  if (written != len) {
    FATAL() << "Failed to write gdb command file";
  }

  stringstream procfile;
  procfile << "/proc/" << getpid() << "/fd/" << fd;
  return procfile.str();
}

string to_shell_string(const vector<string>& args) {
  stringstream ss;
  for (auto& a : args) {
    ss << "'" << a << "' ";
  }
  return ss.str();
}

static bool needs_target(const string& option) {
  return !strncmp(option.c_str(), "continue", option.size());
}

/**
 * Exec the debuger using the params that were written to
 * `params_pipe_fd`.
 */
void launch_debugger(ScopedFd& params_pipe_fd,
                     const string& debugger_file_path,
                     const vector<string>& options,
                     bool serve_files) {
  auto macros = gdb_rr_macros();
  string gdb_command_file = create_gdb_command_file(macros);

  DebuggerParams params;
  ssize_t nread;
  while (true) {
    nread = read(params_pipe_fd, &params, sizeof(params));
    if (nread == 0) {
      // pipe was closed. Probably rr failed/died.
      return;
    }
    if (nread != -1 || errno != EINTR) {
      break;
    }
  }
  DEBUG_ASSERT(nread == sizeof(params));

  vector<string> args;
  args.push_back(debugger_file_path);
  push_default_gdb_options(args, serve_files);
  args.push_back("-x");
  args.push_back(gdb_command_file);
  bool did_set_remote = false;
  for (size_t i = 0; i < options.size(); ++i) {
    if (!did_set_remote && options[i] == "-ex" &&
        i + 1 < options.size() && needs_target(options[i + 1])) {
      push_gdb_target_remote_cmd(args, string(params.host), params.port);
      did_set_remote = true;
    }
    args.push_back(options[i]);
  }
  if (!did_set_remote) {
    push_gdb_target_remote_cmd(args, string(params.host), params.port);
  }
  args.push_back(params.exe_image);

  vector<string> env = current_env();
  env.push_back("GDB_UNDER_RR=1");

  LOG(debug) << "launching " << to_shell_string(args);

  StringVectorToCharArray c_args(args);
  StringVectorToCharArray c_env(env);
  execvpe(debugger_file_path.c_str(), c_args.get(), c_env.get());
  CLEAN_FATAL() << "Failed to exec " << debugger_file_path << ".";
}

void emergency_debug(Task* t) {
  // See the comment in |guard_overshoot()| explaining why we do
  // this.  Unlike in that context though, we don't know if |t|
  // overshot an internal breakpoint.  If it did, cover that
  // breakpoint up.
  if (t->vm()) {
    t->vm()->remove_all_breakpoints();
  }

  // Don't launch a debugger on fatal errors; the user is most
  // likely already in a debugger, and wouldn't be able to
  // control another session. Instead, launch a new GdbServer and wait for
  // the user to connect from another window.
  GdbServerConnection::Features features;
  // Don't advertise reverse_execution to gdb because a) it won't work and
  // b) some gdb versions will fail if the user doesn't turn off async
  // mode (and we don't want to require users to do that)
  features.reverse_execution = false;
  unsigned short port = t->tid;
  ScopedFd listen_fd = open_socket(localhost_addr, &port, PROBE_PORT);

  dump_rr_stack();

  char* test_monitor_pid = getenv("RUNNING_UNDER_TEST_MONITOR");
  if (test_monitor_pid) {
    pid_t pid = atoi(test_monitor_pid);
    // Tell test-monitor to wake up and take a snapshot. It will also
    // connect the emergency debugger so let that happen.
    FILE* gdb_cmd = fopen("gdb_cmd", "w");
    if (gdb_cmd) {
      fputs(to_shell_string(
          debugger_launch_command(t, localhost_addr, port, false, "gdb")).c_str(), gdb_cmd);
      fclose(gdb_cmd);
    }
    kill(pid, SIGURG);
  } else {
    vector<string> cmd = debugger_launch_command(t, localhost_addr, port,
        false, "gdb");
    fprintf(stderr, "Launch debugger with\n  %s\n", to_shell_string(cmd).c_str());
  }
  unique_ptr<GdbServerConnection> dbg =
      GdbServerConnection::await_connection(t, listen_fd, features);
  GdbServer::serve_emergency_debugger(std::move(dbg), t);
}

string gdb_init_script() { return gdb_rr_macros(); }

} // namespace rr
