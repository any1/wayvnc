#!/usr/bin/env python

import asyncio
import json
import re
import os
import glob
import socket

class Program:
	command_seq = 0
	ctl_socket = None
	reader = None
	writer = None
	read_buffer = ""
	decoder = json.JSONDecoder()
	replies = {}
	attaching = False

	async def read_message(self):
		while True:
			try:
				result, index = self.decoder.raw_decode(self.read_buffer)
				self.read_buffer = self.read_buffer[index:].lstrip()
				return result
			except json.JSONDecodeError:
				data = await self.reader.read(4096)
				if not data:
					raise ConnectionError("wayvnc closed the control socket")
				self.read_buffer += data.decode('utf-8')

	async def send_command(self, method, params = None, fd = None):
		command_id = self.command_seq
		self.command_seq += 1

		cmd = {
			"method": method,
			"id": command_id,
		}

		if not params is None:
			cmd['params'] = params

		data = json.dumps(cmd).encode()
		if fd is None:
			self.writer.write(data)
			await self.writer.drain()
		else:
			await self.writer.drain()
			socket.send_fds(self.ctl_socket, [data], [fd])

		while command_id not in self.replies:
			message = await self.read_message()
			if 'code' in message and 'id' in message:
				self.replies[message['id']] = message
			else:
				await self.process_message(message)

		return self.replies.pop(command_id)['code'] == 0

	async def attach(self, display):
		display_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		try:
			display_socket.connect(display)
		except OSError:
			display_socket.close()
			return False

		with display_socket:
			self.attaching = True
			result = await self.send_command('attach', None,
					display_socket.fileno())
			self.attaching = False
		return result

	async def attach_any(self):
		for path in glob.iglob('/run/user/*/wayland-*'):
			if path.endswith('.lock'):
				continue

			if await self.attach(path):
				return True

		return False

	async def attach_any_with_retry(self):
		while not await self.attach_any():
			await asyncio.sleep(1.0)

	async def process_message(self, message):
		method = message['method']
		if (method == 'detached'):
			# Switching attachment detaches from the old compositor first;
			# ignore the detached event our own attach produces.
			if not self.attaching:
				await self.attach_any_with_retry()

	async def main(self):
		self.ctl_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.ctl_socket.connect("/tmp/wayvnc/wayvncctl.sock")
		self.reader, self.writer = await asyncio.open_unix_connection(
				sock=self.ctl_socket)

		await self.attach_any_with_retry()
		await self.send_command("event-receive")

		while True:
			message = await self.read_message()
			await self.process_message(message)

prog = Program()
asyncio.run(prog.main())
