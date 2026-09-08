bindir = get_option('bindir')
session_share = data_dir / 'telescope-session'

# Paths get substituted at configure time. Writing /usr literally would break
# any prefix that is not /usr, which is every developer install.
session_conf = configuration_data()
session_conf.set('bindir', prefix / bindir)
session_conf.set('sessiondir', prefix / session_share)

# Display-manager entry point.
install_data('telescope-session', install_dir : bindir, install_mode : 'rwxr-xr-x')
install_data('telescope-session-select', install_dir : bindir, install_mode : 'rwxr-xr-x')

# Steam's Switch to Desktop button runs this by name. Shipping it means the
# session package conflicts with gamescope-session-steam, which is fine: two
# gamemode sessions cannot both own the display anyway.
install_data('steamos-session-select', install_dir : bindir, install_mode : 'rwxr-xr-x')

# The session proper, run by the unit. Not on PATH: it is not a user command.
configure_file(
  input         : 'telescope-session-run.in',
  output        : 'telescope-session',
  configuration : session_conf,
  install       : true,
  install_dir   : session_share,
  install_mode  : 'rwxr-xr-x',
)

install_data('sessions.d/steam', install_dir : session_share / 'sessions.d')
install_data('device-quirks', install_dir : session_share)

install_data('telescope-steam.desktop', install_dir : data_dir / 'wayland-sessions')

# systemd unit dirs are never multiarch: it searches <prefix>/lib/systemd/user,
# so a Debian libdir of lib/x86_64-linux-gnu would put this where nothing reads.
configure_file(
  input         : 'telescope-session@.service.in',
  output        : 'telescope-session@.service',
  configuration : session_conf,
  install       : true,
  install_dir   : prefix / 'lib' / 'systemd' / 'user',
)
