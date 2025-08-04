# FAQ

**Q: How can I run wayvnc in headless mode/over an SSH session?**

A: Set the environment variables `WLR_BACKENDS=headless` and
`WLR_LIBINPUT_NO_DEVICES=1` before starting sway, then set
`WAYLAND_DISPLAY=wayland-1` and run wayvnc. For older versions of sway,
`WAYLAND_DISPLAY` is `wayland-0`. Try that if `wayland-1` doesn't work.

**Q: How can I pass my mod-key from Sway to the remote desktop session?**

A: Create an almost empty mode in your sway config. Example:
```
mode passthrough {
	bindsym $mod+Pause mode default
}
bindsym $mod+Pause mode passthrough
```
This makes it so that when you press $mod+Pause, all keybindings, except the one
to switch back, are disabled.

Disable `floating_modifier` during the mode if it's set up in your config file
and you wish to be able to use the same functionality in the nested desktop:
```
mode passthrough {
    bindsym $mod+Pause mode default; floating_modifier $mod normal
}
bindsym $mod+Pause mode passthrough; floating_modifier none
```
Replace `$mod normal` with different arguments if applicable.

**Q: Not all symbols show up when I'm typing. What can I do to fix this?**

A: Try setting the keyboard layout in wayvnc to the one that most closely
matches the keyboard layout that you're using on the client side. An exact
layout isn't needed, just one that has all the symbols that you use.

**Q: How do i connect to a running Wayland session?**

A: Let's say you want to control the host at `$host` which is already up and running Wayland:

  * `ssh -L 5900:localhost:5900 $user@$host WAYLAND_DISPLAY=wayland-1 wayvnc localhost`
  * Now, connect from your local device (VNC tunneled through SSH session): `vncviewer localhost:5900`
