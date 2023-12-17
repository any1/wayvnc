#!/usr/bin/env python

import asyncio
import json
import re
import os
import glob
from pathlib import Path

class Program:
	command_seq = 0
	reader = None
	writer = None
	read_buffer = ""
	message_queue = asyncio.Queue()
	reply_queue = asyncio.Queue()
	decoder = json.JSONDecoder()
	tasks = []

	async def read_message(self):
		while True:
			try:
				result, index = self.decoder.raw_decode(self.read_buffer)
				self.read_buffer = self.read_buffer[index:].lstrip()
				return result
			except json.JSONDecodeError:
				data = await self.reader.read(4096)
				self.read_buffer += data.decode('utf-8')

	async def send_command(self, method, params = None):
		cmd = {
			"method": method,
			"id": self.command_seq,
		}

		if not params is None:
			cmd['params'] = params

		self.command_seq += 1
		self.writer.write(json.dumps(cmd).encode())
		await self.writer.drain()

		reply = await self.reply_queue.get()
		self.reply_queue.task_done()
		return reply['code'] == 0

	async def attach(self, display):
		# TODO: It would be better to pass the socket on to wayvnc as a file descriptor
		proc = await asyncio.create_subprocess_shell('setfacl -m "u:vnc:rwx" {} {}'.format(Path(display).parent, display))
		await proc.wait()
		return await self.send_command('attach', {'display': display})

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
			await self.attach_any_with_retry()

	async def message_processor(self):
		while True:
			message = await self.read_message()
			if 'method' in message:
				await self.message_queue.put(message)
			elif 'code' in message:
				await self.reply_queue.put(message)

	async def main(self):
		self.reader, self.writer = await asyncio.open_unix_connection("/tmp/wayvnc/wayvncctl.sock")
		self.tasks.append(asyncio.create_task(self.message_processor()))

		await self.attach_any_with_retry()
		await self.send_command("event-receive")

		while True:
			message = await self.message_queue.get()
			await self.process_message(message)

prog = Program()
asyncio.run(prog.main())
