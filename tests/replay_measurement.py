"""Validate rejection of misleading replay throughput comparisons."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'measurement', Path(__file__).resolve().parents[1] / 'scripts/measure_windows_replay.py')
measurement = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measurement)


def samples():
    return [dict(time=t, frame=f, tps=240., speed=1., play_layer=42,
                 inputs=i, display_frames=d)
            for t, f, i, d in ((10., 100, 20, 5), (11., 200, 30, 65), (12., 300, 40, 125))]


class MeasurementTests(unittest.TestCase):
    def test_rates(self):
        result = measurement.summarize(samples())
        self.assertEqual(result['steps_per_second'], 100)
        self.assertEqual(result['inputs'], 20)
        self.assertEqual(result['display_frames_per_second'], 60)

    def test_invalid_windows(self):
        for field, value in (('time', 9.), ('frame', 99), ('tps', 120.),
                             ('speed', 2.), ('play_layer', 43), ('inputs', 0),
                             ('display_frames', 0)):
            with self.subTest(field=field):
                data = samples()
                data[1][field] = value
                with self.assertRaises(ValueError):
                    measurement.summarize(data)
        data = samples()
        for sample in data:
            sample['frame'] = 100
        with self.assertRaises(ValueError):
            measurement.summarize(data)

    def test_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / name for name in ('a.json', 'b.json', 'a2.json')]
            runs = [dict(samples=samples(), workload='level/replay', build='hash',
                         settings='mods/display') for _ in paths]
            for sample in runs[1]['samples']:
                sample['time'] = 10 + (sample['time'] - 10) / 2
            def write():
                for path, run in zip(paths, runs):
                    path.write_text(json.dumps(run))
            write()
            self.assertEqual([r['throughput_ratio'] for r in measurement.compare_runs(paths)['runs']],
                             [1, 2, 1])
            for field in ('frame', 'inputs'):
                runs[1]['samples'][-1][field] += 1
                write()
                with self.assertRaises(ValueError):
                    measurement.compare_runs(paths)
                runs[1]['samples'][-1][field] -= 1
            runs[1]['settings'] = 'different mods'
            write()
            with self.assertRaises(ValueError):
                measurement.compare_runs(paths)


if __name__ == '__main__':
    unittest.main()
