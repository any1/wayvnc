#!/usr/bin/env python

import asyncio
import json
import re
import os
import glob

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
		return await self.send_command('attach', {'display': display})

	async def attach_any(self):
		for path in glob.iglob('/run/user/*/wayland-*'):
			if path.endswith('.lock'):
				continue

			if await self.attach(path):
				return True

		return False

	async def handle_detached(self):
		while not await self.attach_any():
			await asyncio.sleep(1.0)

	async def process_message(self, message):
		method = message['method']
		if (method == 'detached'):
			await self.handle_detached()

	async def message_processor(self):
		while True:
			message = await self.read_message()
			if 'method' in message:
				await self.message_queue.put(message)
			elif 'code' in message:
				await self.reply_queue.put(message)

	async def main(self):
		self.reader, self.writer = await asyncio.open_unix_connection("/tmp/wayvncctl-0")
		self.tasks.append(asyncio.create_task(self.message_processor()))

		await self.attach_any()
		await self.send_command("event-receive")

		while True:
			message = await self.message_queue.get()
			await self.process_message(message)

prog = Program()
asyncio.run(prog.main())
