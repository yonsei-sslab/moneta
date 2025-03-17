#!/bin/bash

fifo_base="/tmp/serial-err-vm0"
fifo_in="${fifo_base}.in"
fifo_out="${fifo_base}.out"

# Remove the existing named pipes if they exist
rm -f "$fifo_in"
rm -f "$fifo_out"

# Create the named pipes with the desired permissions
mkfifo "$fifo_in"
chmod 0666 "$fifo_in"

mkfifo "$fifo_out"
chmod 0666 "$fifo_out"