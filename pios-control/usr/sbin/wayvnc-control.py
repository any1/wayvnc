#!/usr/bin/env python

import asyncio
import json
import re
import os
import signal
import glob
import dbus # dependency: python3-dbus
import pamela # dependency: python3-pamela
from pathlib import Path

class Greeter:
	def __init__(self, bus, username, password):
		self.username = username
		self.password = password

		systemd = bus.get_object('org.freedesktop.systemd1', '/org/freedesktop/systemd1')
		unit_path = dbus.Interface(systemd, 'org.freedesktop.systemd1.Manager').GetUnit('greetd.service')
		unit = bus.get_object('org.freedesktop.systemd1', unit_path)
		props = dbus.Interface(unit, 'org.freedesktop.DBus.Properties')
		pid = props.Get('org.freedesktop.systemd1.Service', 'MainPID')
		self.socket_path = f'/run/greetd-{pid}.sock'

	async def send(self, msg):
		data = json.dumps(msg).encode()
		self.writer.write(len(data).to_bytes(4, 'little') + data)
		await self.writer.drain()

	async def recv(self):
		header = await self.reader.readexactly(4)
		return json.loads(await self.reader.readexactly(int.from_bytes(header, 'little')))

	def killall(self, name):
		for pid_path in Path('/proc').glob('[0-9]*/comm'):
			if pid_path.read_text().strip() == name:
				os.kill(int(pid_path.parent.name), signal.SIGTERM)

	async def authenticate(self):
		self.reader, self.writer = await asyncio.open_unix_connection(self.socket_path)
		try:
			await self.send({'type': 'create_session', 'username': self.username})

			while True:
				response = await self.recv()
				if response['type'] == 'auth_message':
					reply = self.password if response['auth_message_type'] == 'secret' else None
					await self.send({'type': 'post_auth_message_response', 'response': reply})
				elif response['type'] == 'success':
					break
				else:
					return False

			# TODO: We should get the correct cmd from some config
			await self.send({'type': 'start_session', 'cmd': ['labwc']})
			response = await self.recv()
			if response['type'] != 'success':
				return False

			# The session runs after the greeter exits, so it needs
			# a little help.
			# TODO: Perhaps figure out the current greeter instead
			# of hardcoding?
			self.killall('gtkgreet')
			self.killall('agreety')

			return True
		finally:
			self.writer.close()
			await self.writer.wait_closed()

class Program:
	command_seq = 0
	reader = None
	writer = None
	read_buffer = ""
	message_queue = asyncio.Queue()
	reply_queue = asyncio.Queue()
	decoder = json.JSONDecoder()
	tasks = []
	bus = dbus.SystemBus()

	# TODO: Is there an async interface in the dbus module?
	def get_session_property(self, session_path, property_name):
		session = self.bus.get_object('org.freedesktop.login1', session_path)
		props = dbus.Interface(session, 'org.freedesktop.DBus.Properties')
		return props.Get('org.freedesktop.login1.Session', property_name)

	def get_active_wayland_session_user(self):
		manager = self.bus.get_object('org.freedesktop.login1', '/org/freedesktop/login1')
		manager_iface = dbus.Interface(manager, 'org.freedesktop.login1.Manager')

		for _session_id, _uid, session_username, _seat, session_path in manager_iface.ListSessions():
			try:
				if self.get_session_property(session_path, 'Type') != 'wayland':
					continue
				if not bool(self.get_session_property(session_path, 'Active')):
					continue
				if self.get_session_property(session_path, 'Class') == 'greeter':
					continue
			except dbus.DBusException:
				continue

			return session_username

		return None

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

	async def auth_accept(self, reply_token):
		return self.send_command('auth-reply', {
			'reply-token': '{}'.fmt(reply_token),
			'accept': '1'})

	async def auth_reject(self, reply_token, reason):
		return self.send_command('auth-reply', {
			'reply-token': '{}'.fmt(reply_token),
			'reject': '1',
			'reason': reason})

	async def handle_auth_via_greeter(self, reply_token, username, password):
		greeter = Greeter(self.bus, username, password)
		if await greeter.authenticate():
			await self.auth_accept(reply_token)
		else:
			await self.auth_reject(reply_token, "Invalid username or password")

	async def handle_auth_via_pam(self, reply_token, username, password):
		if pamela.authenticate(username, password):
			await self.auth_accept(reply_token, username, password)
		else:
			await self.auth_reject(reply_token, "Invalid username or password")

	async def handle_auth_request(self, params):
		reply_token = params['reply-token']
		username = params['username']
		password = params['password']

		active_user = self.get_active_wayland_session_user()
		if active_user is None:
			await self.handle_auth_via_greeter(reply_token, username, password)
		elif active_user == username:
			await self.handle_auth_via_pam(reply_token, username, password)
		else:
			await self.auth_reject(reply_token, "Another user is already logged in")

	async def process_message(self, message):
		method = message['method']
		params = message['params']

		if (method == 'detached'):
			await self.attach_any_with_retry()
		elif (method == 'auth-request'):
			await self.handle_auth_request(params)

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
