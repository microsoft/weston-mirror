if not get_option('backend-rdp')
	subdir_done()
endif

config_h.set('BUILD_RDP_COMPOSITOR', '1')

dep_frdp = dependency('freerdp3', version: '>= 3.0.0', required: false)
if not dep_frdp.found()
	dep_frdp = dependency('freerdp2', version: '>= 2.2.0', required: false)
	if not dep_frdp.found()
		error('RDP-backend requires freerdp >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
	endif
endif

dep_frdp_server = dependency('freerdp-server3', version: '>= 3.0.0', required: false)
if not dep_frdp_server.found()
	dep_frdp_server = dependency('freerdp-server2', version: '>= 2.2.0', required: false)
	if not dep_frdp_server.found()
		error('RDP-backend requires freerdp-server >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
	endif
endif

dep_wpr = dependency('winpr3', version: '>= 3.0.0', required: false)
if not dep_wpr.found()
	dep_wpr = dependency('winpr2', version: '>= 2.2.0', required: false)
	if not dep_wpr.found()
		error('RDP-backend requires winpr >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
	endif
endif

dep_rdpapplist = dependency('rdpapplist', version: '>= 2.0.0', required: false)
if dep_rdpapplist.found()
	config_h.set('HAVE_FREERDP_RDPAPPLIST_H', '1')
endif

if cc.has_header('freerdp/channels/gfxredir.h', dependencies: dep_frdp)
	config_h.set('HAVE_FREERDP_GFXREDIR_H', '1')
endif

if cc.has_member(
	'rdpsnd_server_context', 'use_dynamic_virtual_channel',
	dependencies : dep_frdp,
	prefix : '#include <freerdp/server/rdpsnd.h>'
)
	config_h.set('HAVE_RDPSND_DYNAMIC_VIRTUAL_CHANNEL', '1')
endif

deps_rdp = [
	dep_threads,
	dep_libdl,
	dep_libweston_private,
	dep_frdp,
	dep_frdp_server,
	dep_wpr,
	dep_rdpapplist,
]

dep_fuse3 = dependency('fuse3', version: '>= 3.12', required: false)
if dep_fuse3.found()
	config_h.set('HAVE_FUSE3', '1')
	deps_rdp += dep_fuse3
else
	message('fuse3 not found: RDP drive redirection disabled')
endif

# printing to RDP client printers: CUPS backend, filter, PPD, MIME type
dir_weston_rail_data = join_paths(dir_data, 'weston-rail')
config_h.set_quoted('WESTON_RAIL_DATADIR', dir_weston_rail_data)
dir_cups_serverbin = get_option('cups-serverbin')
dir_cups_datadir = get_option('cups-datadir')
executable(
	'rdpprint',
	'cups/rdpprint-backend.c',
	install: true,
	install_dir: join_paths(dir_cups_serverbin, 'backend'),
	install_mode: 'rwx------',
)
install_data('cups/rdpxps',
	install_dir: join_paths(dir_cups_serverbin, 'filter'),
	install_mode: 'rwxr-xr-x')
install_data('cups/weston-rail.types',
	install_dir: join_paths(dir_cups_datadir, 'mime'))
install_data('cups/rdp-xps.ppd', install_dir: dir_weston_rail_data)
install_data('../../data/weston-rail/apps.conf.example', install_dir: dir_weston_rail_data)

dep_openssl = dependency('openssl', version: '>= 1.1.1', required: false)
if dep_openssl.found()
	config_h.set('HAVE_OPENSSL', '1')
	deps_rdp += dep_openssl
endif

srcs_rdp = [
        'hash.c',
        'rdp.c',
        'rdpdisp.c',
        'rdpclip.c',
        'rdprail.c',
        'rdputil.c',
        'rdpdrive.c',
        'rdpdr/rdpdr_server.c',
]

plugin_rdp = shared_library(
	'rdp-backend',
	srcs_rdp,
	include_directories: common_inc,
	dependencies: deps_rdp,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'rdp-backend.so=@0@;'.format(plugin_rdp.full_path())
install_headers(backend_rdp_h, subdir: dir_include_libweston_install)
