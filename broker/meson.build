# weston-rail-broker: RDP session broker (one weston per user)
if not get_option('backend-rdp')
	subdir_done()
endif

dep_pam_broker = cc.find_library('pam', required: false)
if not dep_pam_broker.found() or not cc.has_header('security/pam_appl.h')
	message('libpam not found: weston-rail-broker not built')
	subdir_done()
endif

executable(
	'weston-rail-broker',
	'weston-rail-broker.c',
	include_directories: common_inc,
	c_args: [ '-DWESTON_BINARY="@0@"'.format(join_paths(dir_bin, 'weston')) ],
	dependencies: [ dep_frdp, dep_frdp_server, dep_wpr, dep_pam_broker, dep_threads ],
	install: true,
	install_dir: join_paths(dir_prefix, get_option('sbindir')),
)

executable(
	'weston-rail-passwd',
	'weston-rail-passwd.c',
	include_directories: common_inc,
	dependencies: [ dep_wpr, dep_pam_broker ],
	install: true,
	install_dir: join_paths(dir_prefix, get_option('sbindir')),
)

executable(
	'weston-rail-sessions',
	'weston-rail-sessions.c',
	include_directories: common_inc,
	install: true,
	install_dir: join_paths(dir_prefix, get_option('sbindir')),
)

dep_openssl_feed = dependency('openssl', required: false)
if dep_openssl_feed.found()
	executable(
		'weston-rail-feed',
		'weston-rail-feed.c',
		include_directories: common_inc,
		dependencies: [ dep_openssl_feed, dep_threads ],
		install: true,
		install_dir: join_paths(dir_prefix, get_option('sbindir')),
	)
	feed_service_conf = configuration_data()
	feed_service_conf.set('sbindir', join_paths(dir_prefix, get_option('sbindir')))
	configure_file(
		input: 'weston-rail-feed.service.in',
		output: 'weston-rail-feed.service',
		configuration: feed_service_conf,
		install_dir: join_paths(dir_prefix, 'lib', 'systemd', 'system'),
	)
endif

shared_module(
	'pam_weston_rail',
	'pam_weston_rail.c',
	name_prefix: '',
	include_directories: common_inc,
	c_args: [ '-DWESTON_RAIL_PASSWD="@0@"'.format(
		join_paths(dir_prefix, get_option('sbindir'), 'weston-rail-passwd')) ],
	dependencies: [ dep_pam_broker ],
	install: true,
	install_dir: join_paths(dir_lib, 'security'),
)

install_data('weston-rail.pam',
	rename: 'weston-rail',
	install_dir: '/etc/pam.d')

broker_service_conf = configuration_data()
broker_service_conf.set('sbindir', join_paths(dir_prefix, get_option('sbindir')))
configure_file(
	input: 'weston-rail-broker.service.in',
	output: 'weston-rail-broker.service',
	configuration: broker_service_conf,
	install_dir: join_paths(dir_prefix, 'lib', 'systemd', 'system'),
)
