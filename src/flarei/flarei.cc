/**
 *	flarei.cc
 *
 *	implementation of gree::flare::flarei (and some other global stuffs)
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#include "flarei.h"

namespace gree {
namespace flare {

// {{{ global functions
/**
 *	signal handler (SIGTERM/SIGINT)
 */
void sa_term_handler(int sig) {
	if (sig != SIGTERM && sig != SIGINT) {
		return;
	}
	log_notice("received signal [%s] -> requesting shutdown", sig == SIGTERM ? "SIGTERM" : "SIGINT");

	singleton<flarei>::instance().request_shutdown();

	return;
}

/**
 *	signal handler (SIGHUP)
 */
void sa_hup_handler(int sig) {
	if (sig != SIGHUP) {
		return;
	}
	log_notice("received signal [SIGHUP] -> reloading", 0);

	singleton<flarei>::instance().reload();

	return;
}

/**
 *	signal handler (SIGUSR1)
 */
void sa_usr1_handler(int sig) {
	log_notice("received signal [SIGUSR1]", 0);

	// just interrupting -> nothing to do
}
// }}}

// {{{ ctor/dtor
/**
 *	ctor for flarei
 */
flarei::flarei():
		_server(NULL),
		_thread_pool(NULL),
		_cluster(NULL) {
}

/**
 *	dtor for flarei
 */
flarei::~flarei() {
	if (this->_server != NULL) {
		_delete_(this->_server);
	}
	if (this->_thread_pool != NULL) {
		_delete_(this->_thread_pool);
	}
	if (this->_cluster != NULL) {
		_delete_(this->_cluster);
	}
	if (stats_object != NULL) {
		_delete_(stats_object);
		stats_object = NULL;
	}

#ifdef MM_ALLOCATION_CHECK
	mm::dump_alloc_list();
#endif
}
// }}}

// {{{ operator overloads
// }}}

// {{{ public methods
/**
 *	flarei application startup procs
 */
int flarei::startup(int argc, char **argv) {
	ini_option_object().set_args(argc, argv);
	if (ini_option_object().load() < 0) {
		return -1;
	}

	singleton<logger>::instance().open(this->_ident, ini_option_object().get_log_facility());
	stats_object = _new_ stats_index();
	stats_object->startup();

	log_notice("%s version %s - system logger started", this->_ident.c_str(), PACKAGE_VERSION);

	log_notice("application startup in progress...", 0);
	log_notice("  config_path:          %s", ini_option_object().get_config_path().c_str());
	log_notice("  daemonize:            %s", ini_option_object().is_daemonize() ? "true" : "false");
	log_notice("  data_dir:             %s", ini_option_object().get_data_dir().c_str());
	log_notice("  max_connection:       %d", ini_option_object().get_max_connection());
	log_notice("  monitor_threshold:    %d", ini_option_object().get_monitor_threshold());
	log_notice("  monitor_interval:     %d", ini_option_object().get_monitor_interval());
	log_notice("  monitor_read_timeout: %d", ini_option_object().get_monitor_read_timeout());
	log_notice("  net_read_timeout:     %d", ini_option_object().get_net_read_timeout());
	log_notice("  server_name:          %s", ini_option_object().get_server_name().c_str());
	log_notice("  server_port:          %d", ini_option_object().get_server_port());
	log_notice("  stack_size:           %d", ini_option_object().get_stack_size());
	log_notice("  thread_pool_size:     %d", ini_option_object().get_thread_pool_size());

	// startup procs
	if (this->_set_resource_limit() < 0) {
		return -1;
	}

	if (ini_option_object().is_daemonize()) {
		if (this->_daemonize() < 0) {
			return -1;
		}
	}

	if (this->_set_signal_handler() < 0) {
		return -1;
	}

	// application objects
	connection::read_timeout = ini_option_object().get_net_read_timeout() * 1000;		// -> msec
	this->_server = _new_ server();
	if (this->_server->listen(ini_option_object().get_server_port()) < 0) {
		return -1;
	}

	this->_thread_pool = _new_ thread_pool(ini_option_object().get_thread_pool_size(), ini_option_object().get_stack_size());

	this->_cluster = _new_ cluster(this->_thread_pool, ini_option_object().get_data_dir(), ini_option_object().get_server_name(), ini_option_object().get_server_port());
	this->_cluster->set_monitor_threshold(ini_option_object().get_monitor_threshold());
	this->_cluster->set_monitor_interval(ini_option_object().get_monitor_interval());
	this->_cluster->set_monitor_read_timeout(ini_option_object().get_monitor_read_timeout());
	if (this->_cluster->startup_index() < 0) {
		return -1;
	}

	shared_thread th = this->_thread_pool->get(thread_pool::thread_type_alarm);
	handler_alarm* h = _new_ handler_alarm(th);
	th->trigger(h);

	if (this->_set_pid() < 0) {
		return -1;
	}

	return 0;
}

/**
 *	flarei application running loop
 */
int flarei::run() {
	log_notice("entering running loop", 0);

	for (;;) {
		if (this->_shutdown_request) {
			log_notice("shutdown request accepted -> breaking running loop", 0);
			break;
		}

		vector<shared_connection> connection_list = this->_server->wait();
		vector<shared_connection>::iterator it;
		for (it = connection_list.begin(); it != connection_list.end(); it++) {
			shared_connection c = *it;

			if (this->_thread_pool->get_thread_size(thread_pool::thread_type_request) >= ini_option_object().get_max_connection()) {
				log_warning("too many connection [%d] -> closing socket and continue", ini_option_object().get_max_connection());
				continue;
			}

			stats_object->increment_total_connections();

			shared_thread t = this->_thread_pool->get(thread_pool::thread_type_request);
			handler_request* h = _new_ handler_request(t, c);
			t->trigger(h);
		}
	}

	return 0;
}

/**
 *	flarei application reload procs
 */
int flarei::reload() {
	if (ini_option_object().reload() < 0) {
		log_notice("invalid config file -> skip reloading", 0);
		return -1;
	}

	// log_facility
	log_notice("re-opening syslog...", 0);
	singleton<logger>::instance().close();
	singleton<logger>::instance().open(this->_ident, ini_option_object().get_log_facility());

	this->_cluster->set_monitor_threshold(ini_option_object().get_monitor_threshold());
	this->_cluster->set_monitor_interval(ini_option_object().get_monitor_interval());
	this->_cluster->set_monitor_read_timeout(ini_option_object().get_monitor_read_timeout());

	// net_read_timeout
	connection::read_timeout = ini_option_object().get_net_read_timeout() * 1000;		// -> msec

	// thread_pool_size
	this->_thread_pool->set_max_pool_size(ini_option_object().get_thread_pool_size());

	// re-setup resource limit (do not care about return value here)
	this->_set_resource_limit();

	log_notice("process successfully reloaded", 0);

	return 0;
}

/**
 *	flarei application shutdown procs
 */
int flarei::shutdown() {
	log_notice("shutting down active, and pool threads...", 0);
	this->_thread_pool->shutdown();
	log_notice("all threads are successfully shutdown", 0);

	this->_clear_pid();

	return 0;
}
// }}}

// {{{ protected methods
string flarei::_get_pid_path() {
	return ini_option_object().get_data_dir() + "/" + this->_ident + ".pid";
};
// }}}

// {{{ private methods
/**
 *	set resource limit
 */
int flarei::_set_resource_limit() {
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
		log_err("getrlimit() failed: %s (%d)", util::strerror(errno), errno);
		return -1;
	} else {
		rlim_t fd_limit = ini_option_object().get_max_connection() * 2 + 16;
		if (rl.rlim_cur < fd_limit) {
			rl.rlim_cur = fd_limit;
			if (rl.rlim_max < rl.rlim_cur) {
				rl.rlim_max = rl.rlim_cur;
			}
			if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
				log_err("setrlimit() failed: %s (%d) - please run as root", util::strerror(errno), errno);
				return -1;
			}
		}
		log_info("setting resource limit (RLIMIT_NOFILE): %d", fd_limit);
	}

	return 0;
}

/**
 *	setup signal handler(s)
 */
int flarei::_set_signal_handler() {
	struct sigaction sa;

	// SIGTERM/SIGINT
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_term_handler;
	if (sigaction(SIGTERM, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGTERM, util::strerror(errno), errno);
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_term_handler;
	if (sigaction(SIGINT, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGINT, util::strerror(errno), errno);
		return -1;
	}
	log_info("set up sigterm/sigint handler", 0);

	// SIGHUP
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_hup_handler;
	if (sigaction(SIGHUP, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGHUP, util::strerror(errno), errno);
		return -1;
	}
	log_info("set up sighup handler", 0);

	// SIGUSR1
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_usr1_handler;
	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGUSR1, util::strerror(errno), errno);
		return -1;
	}
	log_info("set up sigusr1 handler", 0);

	// signal mask
	sigset_t ss;
	sigfillset(&ss);
	sigdelset(&ss, SIGTERM);
	sigdelset(&ss, SIGINT);
	sigdelset(&ss, SIGHUP);
	if (sigprocmask(SIG_SETMASK, &ss, NULL) < 0) {
		log_err("sigprocmask() failed: %s (%d)", util::strerror(errno), errno);
	}

	return 0;
}
// }}}

}	// namespace flare
}	// namespace gree

// {{{ ::main (entry point)
int main(int argc, char **argv) {
	gree::flare::flarei& f = gree::flare::singleton<gree::flare::flarei>::instance();
	f.set_ident("flarei");

	if (f.startup(argc, argv) < 0) {
		return -1;
	}
	int r = f.run();
	f.shutdown();

	return r;
}
// }}}

// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent
