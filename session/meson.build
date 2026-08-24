bindir = get_option('bindir')
session_share = data_dir / 'telescope-session'

# Display-manager entry point.
install_data('telescope-session', install_dir : bindir, install_mode : 'rwxr-xr-x')
install_data('telescope-session-select', install_dir : bindir, install_mode : 'rwxr-xr-x')

# Steam's Switch to Desktop button runs this by name. Shipping it means the
# session package conflicts with gamescope-session-steam, which is fine: two
# gamemode sessions cannot both own the display anyway.
install_data('steamos-session-select', install_dir : bindir, install_mode : 'rwxr-xr-x')

# The session proper, run by the unit. Not on PATH: it is not a user command.
install_data(
  'telescope-session-run',
  install_dir  : session_share,
  install_mode : 'rwxr-xr-x',
  rename       : 'telescope-session',
)

install_data('sessions.d/steam', install_dir : session_share / 'sessions.d')

install_data('telescope-steam.desktop', install_dir : data_dir / 'wayland-sessions')

# systemd unit dirs are never multiarch: it searches <prefix>/lib/systemd/user,
# so a Debian libdir of lib/x86_64-linux-gnu would put this where nothing reads.
install_data('telescope-session@.service', install_dir : prefix / 'lib' / 'systemd' / 'user')
