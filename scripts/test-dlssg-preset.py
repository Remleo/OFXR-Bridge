"""Compare native OFXR DLSS FG A/C using an isolated, temporary NVIDIA profile.

Requires 64-bit Windows Python, a native-enabled xrfg_d3d12_history_tests.exe,
and NVIDIA app's beta FG runtime. No SDK download or third-party Python package
is needed. Only the uniquely named copy of the test executable gets an override.
"""

import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import uuid


# Public NVAPI declarations and query IDs:
# https://github.com/NVIDIA/nvapi/blob/main/nvapi.h
# https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h
# https://github.com/NVIDIA/nvapi/blob/main/NvApiDriverSettings.h
U32, HANDLE = ct.c_uint32, ct.c_void_p
WSTRING = ct.c_wchar * 2048
FG_OVERRIDE = 0x10E41E03
FG_PRESET = 0x10E41DF1


class Value(ct.Union):
    _pack_ = 4
    _fields_ = [('dword', U32), ('storage', ct.c_byte * 4100)]


class Setting(ct.Structure):
    _pack_ = 4
    _fields_ = [('version', U32), ('name', WSTRING), ('id', U32),
                ('type', U32), ('location', U32), ('predefined', U32),
                ('valid', U32), ('default', Value), ('current', Value)]


class Application(ct.Structure):
    _fields_ = [('version', U32), ('predefined', U32), ('name', WSTRING),
                ('friendly_name', WSTRING), ('launcher', WSTRING)]


class Profile(ct.Structure):
    _fields_ = [('version', U32), ('name', WSTRING), ('gpu', U32),
                ('predefined', U32), ('applications', U32), ('settings', U32)]


def versioned(kind):
    value = kind()
    value.version = ct.sizeof(kind) | (1 << 16)
    return value


class TemporaryProfile:
    def __init__(self, executable_name, profile_name):
        self.name = profile_name
        self.executable_name = executable_name
        self.session, self.profile = HANDLE(), HANDLE()
        self.created = False
        # Do not search the current directory for driver libraries.
        self.library = ct.WinDLL('nvapi64.dll', winmode=0x00000800)
        query = self.library.nvapi_QueryInterface
        query.argtypes, query.restype = [U32], HANDLE
        declarations = {
            'initialize': (0x0150E828, ()),
            'create_session': (0x0694D52E, (ct.POINTER(HANDLE),)),
            'destroy_session': (0xDAD9CFF8, (HANDLE,)),
            'load': (0x375DBD6B, (HANDLE,)),
            'save': (0xFCBC7E14, (HANDLE,)),
            'create_profile': (0xCC176068, (HANDLE, ct.POINTER(Profile), ct.POINTER(HANDLE))),
            'find_profile': (0x7E4A9A0B, (HANDLE, ct.c_wchar_p, ct.POINTER(HANDLE))),
            'delete_profile': (0x17093206, (HANDLE, HANDLE)),
            'create_application': (0x4347A9DE, (HANDLE, HANDLE, ct.POINTER(Application))),
            'set': (0x577DD202, (HANDLE, HANDLE, ct.POINTER(Setting))),
            'get': (0x73BF8338, (HANDLE, HANDLE, U32, ct.POINTER(Setting))),
        }
        self.functions = {}
        for name, (identifier, signature) in declarations.items():
            pointer = query(identifier)
            if not pointer:
                raise RuntimeError(f'NVAPI {name} is unavailable')
            self.functions[name] = ct.CFUNCTYPE(ct.c_int, *signature)(pointer)

    def call(self, name, *args):
        status = self.functions[name](*args)
        if status:
            raise RuntimeError(f'NVAPI {name} failed: {status}')

    def __enter__(self):
        try:
            self.call('initialize')
            self.call('create_session', ct.byref(self.session))
            self.call('load', self.session)
            profile = versioned(Profile)
            profile.name = self.name
            self.call('create_profile', self.session, ct.byref(profile), ct.byref(self.profile))
            self.created = True
            app = versioned(Application)
            app.name = self.executable_name
            self.call('create_application', self.session, self.profile, ct.byref(app))
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def select(self, preset):
        # Refresh first so a save cannot overwrite unrelated edits made in the
        # NVIDIA app while a previous benchmark was running.
        if getattr(self, 'saved', False):
            self.refresh()
        for identifier, value in [(FG_OVERRIDE, 1), (FG_PRESET, preset)]:
            setting = versioned(Setting)
            setting.id, setting.current.dword = identifier, value
            self.call('set', self.session, self.profile, ct.byref(setting))
        self.call('save', self.session)
        self.saved = True
        self.call('load', self.session)
        self.call('find_profile', self.session, self.name, ct.byref(self.profile))
        for identifier, expected in [(FG_OVERRIDE, 1), (FG_PRESET, preset)]:
            setting = versioned(Setting)
            self.call('get', self.session, self.profile, identifier, ct.byref(setting))
            if setting.current.dword != expected or setting.location != 0:
                raise RuntimeError('Driver did not retain the experiment profile override')

    def refresh(self):
        # LoadSettings on an existing session can retain an old snapshot
        # after another process saves. Reopen before editing or cleaning up.
        self.call('destroy_session', self.session)
        self.session = HANDLE()
        self.call('create_session', ct.byref(self.session))
        self.call('load', self.session)
        self.call('find_profile', self.session, self.name, ct.byref(self.profile))

    def __exit__(self, *_):
        try:
            if self.created and getattr(self, 'saved', False):
                self.refresh()
                self.call('delete_profile', self.session, self.profile)
                self.call('save', self.session)
                print(f'Removed temporary NVIDIA profile: {self.name}', flush=True)
        finally:
            if self.session:
                self.call('destroy_session', self.session)


def run_case(executable, folder, label, preset, environment, verbose=False):
    # Inherited test switches can select a different test branch or geometry.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('XRFG_TEST_', 'XRFG_NATIVE_DLSSG_'))}
    env.update(environment)
    if verbose:
        env['XRFG_TEST_NATIVE_DLSSG_VERBOSE'] = '1'
    log = folder / (label + '.log')
    with log.open('w', encoding='utf-8') as output:
        result = subprocess.run([str(executable)], env=env, cwd=folder,
                                stdout=output, stderr=subprocess.STDOUT, timeout=180)
    content = log.read_text(encoding='utf-8', errors='replace')
    if result.returncode:
        raise RuntimeError(f'{label} failed ({result.returncode}); see {log}')
    if verbose and f'render preset (requested={preset}, effective={preset})' not in content:
        raise RuntimeError(f'{label}: no confirmation that preset took effect; see {log}')
    rows = []
    for match in re.finditer(r'native bench .*?, ([23])X, (still|turning) head: '
                             r'p10_us=([\d.]+) median_us=([\d.]+)', content):
        rows.append(dict(multiplier=int(match[1]), head=match[2],
                         p10_us=float(match[3]), median_us=float(match[4])))
    if 'XRFG_TEST_NATIVE_DLSSG_BENCH' in environment and len(rows) != 4:
        raise RuntimeError(f'{label}: expected four native benchmark results; see {log}')
    if ('XRFG_TEST_NATIVE_DLSSG' in environment and
            'Native DLSS FG stereo, real-copy, reset and missing-depth tests passed' not in content):
        raise RuntimeError(f'{label}: native test completion missing; see {log}')
    quality = [line for line in content.splitlines() if line.startswith('replay native DLSS FG')]
    if 'XRFG_TEST_REPLAY' in environment and len(quality) != 2:
        raise RuntimeError(f'{label}: native replay did not generate at both scales; see {log}')
    runtimes = sorted(set(re.findall(r'feature dlssg snippet: (.+)', content)))
    print(f'{label}: passed; {log}', flush=True)
    return dict(label=label, preset=preset, benchmark=rows, replay=quality, runtimes=runtimes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-exe', type=Path, default=Path('build/Release/xrfg_d3d12_history_tests.exe'))
    parser.add_argument('--output', type=Path, required=True, help='New directory for retained binaries and logs')
    parser.add_argument('--rounds', type=int, default=3, choices=range(1, 11))
    parser.add_argument('--eye', default='3004x3004')
    parser.add_argument('--replay', type=Path, action='append', default=[], help='Optional recorded frame sequence')
    args = parser.parse_args()
    if os.name != 'nt' or ct.sizeof(HANDLE) != 8:
        parser.error('64-bit Windows Python is required')
    if not re.fullmatch(r'[1-9][0-9]{1,3}x[1-9][0-9]{1,3}', args.eye):
        parser.error('--eye must be WIDTHxHEIGHT')
    source = args.test_exe.resolve(strict=True)
    runtime = source.parent / 'nvngx_dlssg.dll'
    if not runtime.is_file():
        parser.error('The native test executable must have nvngx_dlssg.dll beside it')
    replays = [path.resolve(strict=True) for path in args.replay]
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    token = uuid.uuid4().hex
    executable = folder / f'ofxr_fg_{token}.exe'
    shutil.copy2(source, executable)
    shutil.copy2(runtime, folder / runtime.name)
    profile_name = 'OFXR DLSS FG validation ' + token
    report = dict(profile=profile_name, executable_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  bundled_runtime_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
                  eye=args.eye, cases=[], completed=False)
    # Write before any driver mutation so an interrupted run leaves its unique
    # profile name and binary identity available for diagnosis.
    report_path = folder / 'results.json'
    report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    try:
        with TemporaryProfile(executable.name, profile_name) as profile:
            for preset in [1, 3]:
                profile.select(preset)
                report['cases'].append(run_case(executable, folder, f'quality-{preset}', preset,
                                                {'XRFG_TEST_NATIVE_DLSSG': '1'}, verbose=True))
            for round_number in range(1, args.rounds + 1):
                for preset in ([1, 3] if round_number % 2 else [3, 1]):
                    profile.select(preset)
                    report['cases'].append(run_case(executable, folder, f'bench-{round_number}-{preset}', preset,
                        {'XRFG_TEST_NATIVE_DLSSG_BENCH': '1', 'XRFG_TEST_BENCH_EYE': args.eye}))
            for number, replay in enumerate(replays):
                for preset in [1, 3]:
                    profile.select(preset)
                    report['cases'].append(run_case(executable, folder, f'replay-{number}-{preset}', preset,
                        {'XRFG_TEST_REPLAY': str(replay), 'XRFG_TEST_REPLAY_NATIVE_ONLY': '1',
                         'XRFG_TEST_REPLAY_NATIVE_SCALES': '100,67'}, verbose=True))
        report['completed'] = True
        report['timing_summary'] = []
        for multiplier in [2, 3]:
            for head in ['still', 'turning']:
                values = {}
                for preset in [1, 3]:
                    values[preset] = statistics.median(row['p10_us'] for case in report['cases']
                        if case['preset'] == preset for row in case['benchmark']
                        if row['multiplier'] == multiplier and row['head'] == head)
                report['timing_summary'].append(dict(multiplier=multiplier, head=head,
                    a_us=values[1], c_us=values[3], gpu_time_reduction_percent=100*(1-values[3]/values[1])))
        print(json.dumps(report['timing_summary'], indent=2))
    finally:
        report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
