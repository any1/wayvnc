# Integration Testing

## Prerequisites

The integration tests currently require that the following tools are installed:
- sway (1.8 or later)
- lsof
- jq
- bash
- vncdotool

Most of these are available in your normal distro package manager, except 
vncdotool which is a python tool and installable via pip:

```
pip install vncdotool
```

## Running

```
./test/integration/integration.sh
```

Two test suites are defined:

### Smoke test

Tests basic functionality such as:
- Can wayvnc start and connect to wayland?
- Does the wayvncctl IPC mechanism work (both control and events)?
- Can a VNC client connect and send a keystroke through to sway?

### Multi-output test

Tests wayvnc with a multi-output sway, including:
- Do we detect additions and removals of outputs?
- Do the wayvncctl commands to cycle and switch outputs work?

