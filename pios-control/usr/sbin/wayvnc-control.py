#!/usr/bin/env python

import asyncio
import codecs
import json
import re
import os
import glob
import socket
import struct

async def run_loginctl(*args):
	proc = await asyncio.create_subprocess_exec('loginctl', *args,
			stdout=asyncio.subprocess.PIPE,
			stderr=asyncio.subprocess.DEVNULL)
	out, _ = await proc.communicate()
	return out.decode().strip() if proc.returncode == 0 else None

async def get_active_session():
	return await run_loginctl('show-seat', 'seat0',
			'--property=ActiveSession', '--value')

async def get_session_property(session, name):
	return await run_loginctl('show-session', session,
			'--property=' + name, '--value')

async def get_session_user_id(session):
	user_id = await get_session_property(session, 'User')
	return int(user_id) if user_id and user_id.isdigit() else None

def find_displays_of(user_id):
	paths = glob.glob('/run/user/{}/wayland-*'.format(user_id))
	return sorted(path for path in paths if not path.endswith('.lock'))

def get_peer_user_id(sock):
	credentials = sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
			struct.calcsize('3i'))
	_, user_id, _ = struct.unpack('3i', credentials)
	return user_id

class Program:
	command_seq = 0
	ctl_socket = None
	reader = None
	writer = None
	read_buffer = ""
	utf8_decoder = codecs.getincrementaldecoder('utf-8')()
	decoder = json.JSONDecoder()
	replies = {}
	attaching = False
	allowed_user = None

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
				self.read_buffer += self.utf8_decoder.decode(data)

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

		return self.replies.pop(command_id)

	async def attach(self, display, user_id):
		display_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		try:
			display_socket.connect(display)
		except OSError:
			display_socket.close()
			return False

		if get_peer_user_id(display_socket) != user_id:
			display_socket.close()
			return False

		with display_socket:
			self.attaching = True
			reply = await self.send_command('attach', None,
					display_socket.fileno())
			self.attaching = False
		return reply['code'] == 0

	def is_client_allowed(self, client):
		return self.allowed_user is None or \
				client.get('username') == self.allowed_user

	async def disconnect_if_disallowed(self, client):
		if not self.is_client_allowed(client):
			await self.send_command('client-disconnect',
					{'id': client['id']})

	async def disconnect_disallowed_clients(self):
		reply = await self.send_command('client-list')
		for client in reply.get('data') or []:
			await self.disconnect_if_disallowed(client)

	async def allow_only_user_of(self, session):
		if await get_session_property(session, 'Class') == 'greeter':
			self.allowed_user = None
		else:
			self.allowed_user = await get_session_property(session,
					'Name') or ''
		await self.disconnect_disallowed_clients()

	async def attach_any(self):
		session = await get_active_session()
		if not session:
			return False

		user_id = await get_session_user_id(session)
		if user_id is None:
			return False

		for path in find_displays_of(user_id):
			if await self.attach(path, user_id):
				await self.allow_only_user_of(session)
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
		elif (method == 'client-connected'):
			await self.disconnect_if_disallowed(message['params'])

	async def main(self):
		self.ctl_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.ctl_socket.connect("/run/wayvnc/wayvncctl.sock")
		self.reader, self.writer = await asyncio.open_unix_connection(
				sock=self.ctl_socket)

		await self.attach_any_with_retry()
		await self.send_command("event-receive")

		while True:
			message = await self.read_message()
			await self.process_message(message)

prog = Program()
asyncio.run(prog.main())
