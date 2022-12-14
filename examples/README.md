# Example Scripts

The scripts here are examples of how you can automate interesting things with the wayvncctl IPC events.

## event-watcher

This is a pretty simple example that just demonstrates how to tie the
`wayvncctl event-receive` event loop into a bash script. It logs when clients
connect and disconnect.

## single-output-sway

This is more purposeful, and implements an idea for multi-output wayland
servers, collapsing all outputs down to one when the first client connects, and
restoring the configuration when the last client exits.

The mechanism used to collapse the outputs depends on the version of sway installed:

- For sway-1.7 and earlier, the script just temporarily disables all outputs
  except the one being captured. This moves all workspaces to the single
  remaining output.

- For sway-1.8 and later, the script creates a temporary virtual output called
  `HEADLESS-[0-9]+' and then disables all physical outputs, which moves all
  workspaces to the virtual output. On disconnect, all original physical
  outputs are re-enabled, and the virtual output is destroyed.
