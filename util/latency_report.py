#!/usr/bin/python

import os
import math

stream = os.popen('perf script -F time,event')

is_in_update_fb = False

class StateTracker:
    def __init__(self, name, src, enter, leave):
        self.is_active = False
        self.name = name
        self.src = src
        self.enter = enter
        self.leave = leave
        self.n = 0
        self.dt_sum = 0.0
        self.dt_square_sum = 0.0
        self.dt_max = 0.0
        self.dt_min = math.inf

    def add_dt(self, dt):
        self.n += 1
        self.dt_sum += dt
        self.dt_square_sum += dt ** 2
        self.dt_max = max(self.dt_max, dt)
        self.dt_min = min(self.dt_min, dt)

    def apply(self, src, event, t):
        if self.is_active:
            if (src, event) == (self.src, self.leave):
                self.is_active = False
                self.add_dt(t - self.t0)
        else:
            if (src, event) == (self.src, self.enter):
                self.is_active = True
                self.t0 = t

    def avg(self):
        return self.dt_sum / self.n

    def var(self):
        return self.dt_square_sum / self.n - self.avg() ** 2

    def stddev(self):
        return math.sqrt(self.var())

    def report(self):
        if self.n == 0:
            return

        print('{}:'.format(self.name))
        print('\tMin, max: {:.1f} ms, {:.1f} ms'.format(self.dt_min * 1e3, self.dt_max * 1e3))
        print('\tAverage, std.dev.: {:.1f} ms, {:.1f} ms'.format(self.avg() * 1e3, self.stddev() * 1e3))

trackers = [
    StateTracker('Framebuffer update', 'sdt_neatvnc', 'update_fb_start', 'update_fb_done'),
    StateTracker('Framebuffer update (only sending)', 'sdt_neatvnc', 'send_fb_start', 'send_fb_done'),
    StateTracker('Screencopy', 'sdt_wayvnc', 'screencopy_start', 'screencopy_ready'),
    StateTracker('Refine damage', 'sdt_wayvnc', 'refine_damage_start', 'refine_damage_end'),
    StateTracker('Render', 'sdt_wayvnc', 'render_start', 'render_end'),
]

for line in stream:
    [t, src, event, _] = line.replace(' ', '').split(':')
    t = float(t)

    for tracker in trackers:
        tracker.apply(src, event, t)

for tracker in trackers:
    tracker.report()
    print()
