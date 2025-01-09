---
name: Bugs
about: Crashes and other bugs
labels: 'bug'

---

### Useful information:
Please, try to gather as much of useful information as possible and follow
these instructions:

- **Version:**
  - Run this command: `wayvnc -V`

- Provide context, including but not limited to
  - Command line arguments
  - Special environment variables, if any
  - jhich wayland compositor are you using? Which version?
  - What VNC client are you using?
  - Did you configure your VNC client in a specific manner?
  - Linux distro/operating system
  - Kernel version
  - Graphics drivers

- Try to reproduce the problem while capturing a **trace log:**
  - `wayvnc -Ltrace | tee wayvnc-bug.log`

- Get the **stack trace**, in case of a crash:
  - If you have `coredumpctl`, you can gather the stack trace after a crash
    using `coredumpctl gdb wayvnc` and then run `bt full` to obtain the stack
    trace.
  - Otherwise, you can either locate the core file and load it into gdb or run
    wayvnc in gdb like so:
    - `gdb --args wayvnc -Ltrace`
  - If the lines mentioning wayvnc, neatvnc or aml have `??`, please compile
    wayvnc and those other projects from source with debug symbols and try
    again.

- Describe how to **reproduce** the problem

- Try to think about your problem beyond these instructions and include
  whatever information that you believe will help to resolve the issue
