# FAQ

**Q: How can I run wayvnc in headless mode/over an SSH session?**

A: Set the environment variables `WLR_BACKENDS=headless` and
`WLR_LIBINPUT_NO_DEVICES=1` before starting sway, then run wayvnc as normal.

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

**Q: Wayvnc changes the Sway's keyboard layout. Can this be fixed?**

A: This happens because of a bug in how the virtual keyboard protocol interacts
with a feature in sway called smart keyboard grouping. This can be remedied by
either putting wayvnc on a separate seat or turning off smart keyboard grouping
in sway.

Putting wayvnc in its own seat is done like this:
```
swaymsg seat wayvnc fallback false # create a new seat
wayvnc --seat=wayvnc
```

Smart keyboard grouping can be turned off like this:
```
swaymsg seat seat0 keyboard_grouping none
```

**Q: Not all symbols show up when I'm typing. What can I do to fix this?**

A: Try setting the keyboard layout in wayvnc to the one that most closely
matches the keyboard layout that you're using on the client side. An exact
layout isn't needed, just one that has all the symbols that you use.
