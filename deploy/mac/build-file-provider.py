#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the signed Deskflow Files companion using the local macOS SDK.

No registration or installation is performed by this script. The companion
uses the same C++ encrypted transfer implementation as the Deskflow core.
"""
import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--helper', type=Path, required=True, help='Built deskflow-file-transfer executable')
parser.add_argument('--identity', required=True, help='Developer ID signing identity')
parser.add_argument('--team', required=True, help='Apple team identifier')
parser.add_argument('--output', type=Path, required=True, help='Output directory')
parser.add_argument('--arch', choices=['arm64', 'x86_64'], default='arm64')
parser.add_argument('--macdeployqt', default='macdeployqt')
parser.add_argument('--qt-libpath', action='append', default=[], help='Additional Qt library search path (repeatable)')
args = parser.parse_args()
source = Path(__file__).resolve().parents[2] / 'src/apps/deskflow-file-provider'
app = args.output.resolve() / 'Deskflow Files.app'
generated = args.output.resolve() / 'file-provider-generated'
# Deploy Qt before embedding the extension: macdeployqt treats an existing
# .appex in PlugIns as a Qt plugin and tries to inspect its Info.plist as Mach-O.
extension = generated / 'ClipboardProvider.appex'
identifier = 'org.deskflow.files'
group = args.team + '.' + identifier

def run(command):
    subprocess.run([str(s) for s in command], check=True)

def plist(path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(values))

generated.mkdir(parents=True, exist_ok=True)
(generated / 'Group.swift').write_text('let fileGroup = ' + '"' + group + '"\n')
for bundle, executable, bid in [(app, 'DeskflowFiles', identifier), (extension, 'ClipboardProvider', identifier + '.provider')]:
    (bundle / 'Contents/MacOS').mkdir(parents=True, exist_ok=True)
    info = dict(CFBundleIdentifier=bid, CFBundleExecutable=executable, CFBundleName='Deskflow Files',
                CFBundleDisplayName='Deskflow Files', CFBundleVersion='1', CFBundleShortVersionString='1.0',
                CFBundlePackageType='APPL' if bundle == app else 'XPC!', LSMinimumSystemVersion='15.0')
    if bundle == extension:
        info['NSExtension'] = dict(NSExtensionPointIdentifier='com.apple.fileprovider-nonui',
                                  NSExtensionPrincipalClass='ClipboardProvider',
                                  NSExtensionFileProviderDocumentGroup=group,
                                  NSExtensionFileProviderSupportsEnumeration=True)
    else:
        info['LSUIElement'] = True
    plist(bundle / 'Contents/Info.plist', info)
    plist(generated / (executable + '.entitlements'), {
        'com.apple.security.app-sandbox': True,
        'com.apple.security.application-groups': [group],
        'com.apple.security.network.client': True,
    })
sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
base = ['xcrun', 'swiftc', '-swift-version', '5', '-sdk', sdk, '-target', args.arch + '-apple-macos15.0',
        '-O', generated / 'Group.swift', source / 'Common.swift']
run(base + [source / 'Provider.swift', '-module-name', 'ClipboardProvider', '-application-extension',
            '-Xlinker', '-e', '-Xlinker', '_NSExtensionMain', '-o', extension / 'Contents/MacOS/ClipboardProvider'])
run(base + [source / 'Host.swift', '-o', app / 'Contents/MacOS/DeskflowFiles'])
helper = app / 'Contents/MacOS/deskflow-file-transfer'
shutil.copy2(args.helper, helper)
embedded_extension = app / 'Contents/PlugIns/ClipboardProvider.appex'
if embedded_extension.exists():
    shutil.rmtree(embedded_extension)
qt_tool = Path(shutil.which(args.macdeployqt) or args.macdeployqt).absolute()
qt_libpaths = [qt_tool.parent.parent / 'lib', *map(Path, args.qt_libpath)]
deploy = [args.macdeployqt, app, '-executable=' + str(helper), '-always-overwrite',
          *['-libpath=' + str(path) for path in qt_libpaths]]
run(deploy)
# Some Qt distributions discover transitive plugin dependencies only after the
# plugin has been copied into the bundle (for example QtSvg for qsvgicon).
run(deploy)
# The receiver belongs to the sandboxed extension. Keep its executable, Qt
# frameworks, plugins and qt.conf within that bundle so it can launch them.
extension_helper = extension / 'Contents/MacOS/deskflow-file-transfer'
shutil.move(helper, extension_helper)
helper = extension_helper
for name in ['Frameworks', 'PlugIns', 'Resources']:
    deployed = app / 'Contents' / name
    if deployed.exists():
        target = extension / 'Contents' / name
        if target.exists():
            shutil.rmtree(target)
        shutil.move(deployed, target)
# Qt rewrites load commands while resolving transitive dependencies. Sign only
# after deployment is complete, including every nested dylib and framework.
for library in sorted((extension / 'Contents').rglob('*.dylib')):
    if not library.is_symlink():
        run(['codesign', '--force', '--options', 'runtime', '--timestamp', '--sign', args.identity, library])
for framework in sorted((extension / 'Contents/Frameworks').rglob('*.framework'),
                        key=lambda path: len(path.parts), reverse=True):
    run(['codesign', '--force', '--options', 'runtime', '--timestamp', '--sign', args.identity, framework])
embedded_extension.parent.mkdir(parents=True, exist_ok=True)
shutil.move(extension, embedded_extension)
extension = embedded_extension
helper = extension / 'Contents/MacOS/deskflow-file-transfer'
# The child inherits its File Provider parent's sandbox and network entitlement.
plist(generated / 'helper.entitlements', {'com.apple.security.app-sandbox': True, 'com.apple.security.inherit': True})
run(['codesign', '--force', '--options', 'runtime', '--timestamp', '--sign', args.identity,
     '--entitlements', generated / 'helper.entitlements', helper])
for bundle, executable in [(extension, 'ClipboardProvider'), (app, 'DeskflowFiles')]:
    run(['codesign', '--force', '--options', 'runtime', '--timestamp', '--sign', args.identity,
         '--entitlements', generated / (executable + '.entitlements'), bundle])
    run(['codesign', '--verify', '--strict', '--verbose=2', bundle])
print(app)
