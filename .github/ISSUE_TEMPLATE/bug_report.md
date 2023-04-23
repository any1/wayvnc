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

- Try to reproduce while capturing a **trace log:**
  - `wayvnc -Ltrace | tee wayvnc-crash.log`

- Get the **stack trace**:
  - If have `coredumpctl`, you can gather the stack trace after a crash using
    `coredumpctl gdb wayvnc` and then run `bt full` to obtain the stack trace.
  - Otherwise, you can either locate the core file and load it into gdb or run
    wayvnc in gdb like so:
    - `gdb --args wayvnc -Ltrace`
  - If the lines mentioning wayvnc, neatvnc or aml have `??`, please compile
    wayvnc and those other projects from source with debug symbols and try
    again.

- Describe how to **reproduce** the problem
