"""Read-only GD 2.2081 replay measurement; use RVAs from the matching PDB.

Sampling does not pause or modify the game. Run CPU profiling separately.
"""
import argparse
import json
import math
import os
import struct
import time
from pathlib import Path


def summarize(samples):
    if len(samples) < 2:
        raise ValueError('at least two samples are required')
    rates = []
    for before, after in zip(samples, samples[1:]):
        duration = after['time'] - before['time']
        if duration <= 0 or after['frame'] < before['frame']:
            raise ValueError('clock did not advance or replay reset during measurement')
        for field in ('tps', 'speed', 'play_layer'):
            if after[field] != before[field]:
                raise ValueError(f'{field} changed during measurement')
        rates.append((after['frame'] - before['frame']) / duration)
    duration = samples[-1]['time'] - samples[0]['time']
    steps = samples[-1]['frame'] - samples[0]['frame']
    if steps == 0:
        raise ValueError('replay did not advance; resume playback before measuring')
    result = dict(duration=duration, steps=steps, start_frame=samples[0]['frame'],
                  end_frame=samples[-1]['frame'], tps=samples[0]['tps'],
                  speed=samples[0]['speed'], steps_per_second=steps / duration,
                  interval_steps_per_second=rates)
    for field in ('inputs', 'display_frames'):
        if field in samples[0]:
            if any(b[field] < a[field] for a, b in zip(samples, samples[1:])):
                raise ValueError(f'{field} counter reset during measurement')
            delta = samples[-1][field] - samples[0][field]
            result[field] = delta
            result[field + '_per_second'] = delta / duration
    return result


def compare_runs(paths):
    runs = [json.loads(Path(path).read_text()) for path in paths]
    summaries = [summarize(run['samples']) for run in runs]
    # Identical endpoints are necessary: nearby frames can have different costs.
    for field in ('start_frame', 'end_frame', 'tps', 'speed', 'inputs'):
        if any(run.get(field) != summaries[0].get(field) for run in summaries):
            raise ValueError(f'runs have different {field}; comparison rejected')
    metadata = ('workload', 'build', 'settings')
    for field in metadata:
        if not runs[0].get(field) or any(run.get(field) != runs[0][field] for run in runs):
            raise ValueError(f'missing or mismatched {field} metadata')
    baseline = summaries[0]['steps_per_second']
    return dict(runs=[dict(path=str(path), **summary,
                          throughput_ratio=summary['steps_per_second'] / baseline)
                      for path, summary in zip(paths, summaries)],
                limitation='Matching counters and metadata do not establish replay-state or visual equivalence.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pid', type=int, nargs='?')
    p.add_argument('--profile')
    p.add_argument('--output', required=True)
    p.add_argument('--state-rva', type=lambda value: int(value, 0))
    p.add_argument('--frame-offset', type=lambda value: int(value, 0), required=False, default=336)
    p.add_argument('--rate-offset', type=lambda value: int(value, 0), required=False, default=356)
    p.add_argument('--speed-offset', type=lambda value: int(value, 0), required=False, default=444)
    p.add_argument('--cost-rva', type=lambda value: int(value, 0), help='Verified RVA of updater StepCost')
    p.add_argument('--diagnostics', action='store_true', help='Current Session layout; verify against matching PDB')
    p.add_argument('--draw-counters-rva', type=lambda value: int(value, 0))
    p.add_argument('--inputs-rva', type=lambda value: int(value, 0),
                   help='RVA of a uint64 cumulative applied-input counter')
    p.add_argument('--display-frames-rva', type=lambda value: int(value, 0),
                   help='RVA of a uint64 cumulative displayed-frame counter')
    p.add_argument('--seconds', type=float, default=3.5)
    p.add_argument('--interval', type=float, default=.5)
    p.add_argument('--wait-game-time', type=float,
                   help='Wait for the next crossing of this replay time; arms after a reset if already past it')
    p.add_argument('--workload', default='')
    p.add_argument('--build', default='', help='running package hash')
    p.add_argument('--settings', default='', help='mod set and display settings')
    p.add_argument('--variant', default='baseline')
    p.add_argument('--compare', nargs='+', metavar='JSON')
    a = p.parse_args()
    if a.compare:
        if len(a.compare) < 2:
            p.error('--compare requires at least two runs')
        result = compare_runs(a.compare)
    else:
        if a.pid is None or a.profile is None or a.state_rva is None:
            p.error('measurement requires pid, --profile and --state-rva')
        if not (math.isfinite(a.seconds) and math.isfinite(a.interval)
                and 0 < a.interval <= a.seconds):
            p.error('require finite seconds >= interval > 0')
        profile = json.loads(Path(a.profile).read_text())
        if profile['pid'] != a.pid:
            p.error('profile PID does not match; collect a new profile after restarting')
        base = next(x[0] for x in profile['images'] if x[2] == 'decks.echo.dll')
        state = base + a.state_rva
        mem = os.open(f'/proc/{a.pid}/mem', os.O_RDONLY)
        try:
            def read(address, fmt):
                return struct.unpack(fmt, os.pread(mem, struct.calcsize(fmt), address))[0]

            def q(address):
                return read(address, '<Q')

            if a.wait_game_time is not None:
                if not math.isfinite(a.wait_game_time) or a.wait_game_time < 0:
                    p.error('--wait-game-time must be finite and nonnegative')
                def replay_time():
                    rate = read(state + a.rate_offset, '<f')
                    if not math.isfinite(rate) or rate <= 0:
                        raise ValueError('invalid replay rate')
                    return read(state + a.frame_offset, '<i') / rate
                armed = replay_time() < a.wait_game_time
                deadline = time.monotonic() + 600
                print(f'Waiting for replay time {a.wait_game_time:g}s', flush=True)
                while time.monotonic() < deadline:
                    current = replay_time()
                    if current < a.wait_game_time:
                        armed = True
                    if armed and current >= a.wait_game_time:
                        break
                    time.sleep(.01)
                else:
                    raise TimeoutError('replay did not cross requested time within 10 minutes')

            samples = []
            started = time.monotonic()
            while True:
                manager = q(0x1406c2ed8)
                play = q(manager + 0x208) if manager else 0
                if not play:
                    raise ValueError('no active play layer')
                layer = q(play + 0xfe8)
                children = q(layer + 208) if layer else 0
                data = q(children + 56) if children else 0
                sample = dict(time=time.monotonic(), frame=read(state + a.frame_offset, '<i'),
                              tps=read(state + a.rate_offset, '<f'), speed=read(state + a.speed_offset, '<f'),
                              play_layer=play, layer_children=read(data, '<I') if data else 0)
                if a.diagnostics:
                    sample.update(backlog=read(state + 400, '<d'),
                                  real_time=read(state + 452, '<?'),
                                  lock_tps=read(state + 453, '<?'),
                                  lock_fps=read(state + 454, '<?'),
                                  batch_steps=read(state + 416, '<I'))
                if a.cost_rva is not None:
                    sample['cost'] = dict(zip(('step', 'update', 'physics', 'other'),
                        struct.unpack('<dddd', os.pread(mem, 32, base + a.cost_rva))))
                for field, rva in (('inputs', a.inputs_rva),
                                   ('display_frames', a.display_frames_rva)):
                    if rva is not None:
                        sample[field] = q(base + rva)
                if a.draw_counters_rva is not None:
                    sample.update(effect_sprites=q(base + a.draw_counters_rva),
                                  draw_calls=q(base + a.draw_counters_rva + 8))
                samples.append(sample)
                remaining = a.seconds - (time.monotonic() - started)
                if remaining <= 0:
                    break
                time.sleep(min(a.interval, remaining))
        finally:
            os.close(mem)
        result = dict(pid=a.pid, samples=samples, **summarize(samples),
                      workload=a.workload, build=a.build, settings=a.settings, variant=a.variant,
                      limitation='Separate live reads are approximate, not atomic snapshots. '
                      'Verify PDB offsets and compare identical replay sections. '
                      'Layer hiding does not isolate all rendering work.')
    Path(a.output).write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'samples'}, indent=2))


if __name__ == '__main__':
    main()
