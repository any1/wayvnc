# FAQ

*Q: How can I run wayvnc in headless mode/over an SSH session?*

A: Set the environment variables `WLR_BACKENDS=headless` and
`WLR_LIBINPUT_NO_DEVICES=1` before starting sway, then run wayvnc as normal.

*Q: How can I pass my mod-key from Sway to the remote desktop session?*

A: Create an almost empty mode in your sway config. Example:
```
mode passthrough {
	bindsym $mod+Pause mode default
}
bindsym $mod+Pause mode passthrough
```
This makes it so that when you press $mod+Pause, all keybindings, except the one
to switch back, are disabled.
