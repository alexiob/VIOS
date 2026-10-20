"""Structured, read-only live-state snapshot composed from existing board queries.

The record keeps the field layout of docs/bring-up/vios-fast-display-preserved.json
('status', 'source', 'stack', 'resources', 'modules', 'boot', 'base',
'module_sources'), so tools/vios_fast_display_state.py can restore from it.
A 'summary' classifies what is live, dirty, staged and persisted. 'persisted'
means the exact name+CRC32 is in the confirmed boot bundle (BOOT-MANIFEST), i.e.
survives a power cycle; a SAVEd package alone does not. Facts the firmware does
not report (manifest on older firmware, MODULE-STAGE pending source) are
explicitly null rather than guessed.

A backend supplies the ModuleClient/WorkbenchClient queries, stacks(),
resources(), evaluate(), _body() and an RLock named lock. No target RAM cost.
"""
import re
import time

FORMAT = 'vios-snapshot/1'
# Pushes then restores BASE, printing it in decimal. Plain 'BASE @ .' always
# prints "10" because a base is shown in itself.
BASE_QUERY = 'BASE @ DUP DECIMAL . BASE !'


def classify_draft(workbench):
    """Return the lifecycle state of the single workbench draft.

    Flags follow kernel/workbench.c: staged implies the exact revision is
    pending; active means the draft equals the live module source; saved is
    only set by a save in this boot and is cleared by any later edit; WB-EDIT
    loads an active module as clean but with saved=0 (persistence unknown).
    """
    if not workbench['draft']:
        return 'none'
    if workbench['staged']:
        return 'staged'
    if workbench['active']:
        if workbench['saved']:
            return 'saved'
        return 'live-unsaved' if workbench['dirty'] else 'live'
    return 'modified' if workbench['dirty'] else 'diverged'


def _bundle(role):
    """{(name, crc32)} for a usable manifest role, set() for no slot, None if unknown.

    One unverified package makes startup reject the whole slot and fall back,
    so its contents say nothing certain about what a power cycle loads.
    """
    if role is None or role['status'] == 'error' or any(m['package'] != 'ok' for m in role['modules']):
        return None
    return {(m['name'], m['crc32']) for m in role['modules']}


def _manifest_error(result, manifest):
    if manifest is None:
        return (result or {}).get('error') or 'boot manifest unavailable'
    errors = [f"{role} {manifest[role]['slot']}: {manifest[role]['error']}"
              for role in ('confirmed', 'candidate') if manifest[role]['status'] == 'error']
    errors += [f"{role} {manifest[role]['slot']}: package {m['name']} {m['package']}"
               for role in ('confirmed', 'candidate') for m in manifest[role]['modules'] if m['package'] != 'ok']
    return '; '.join(errors) or None


def summarize(record):
    modules = record['modules']['modules']
    manifest = record.get('manifest', {}).get('manifest')
    confirmed = _bundle(manifest and manifest['confirmed'])
    candidate = _bundle(manifest and manifest['candidate'])
    active = {m['name']: m for m in modules if m['active']}
    workbench = record['status']['workbench']
    rows = []
    for m in modules:
        missing = [d['name'] for d in m['dependencies']
                   if d['name'] not in active or active[d['name']]['crc32'] != d['crc32']]
        rows.append({'name': m['name'], 'version': m['version'], 'crc32': m['crc32'],
                     'live': m['active'], 'rollback_available': m['previous'],
                     'dependencies_satisfied': not missing, 'unsatisfied_dependencies': missing,
                     'draft': classify_draft(workbench) if workbench['module'] == m['name'] else None,
                     'persisted': None if confirmed is None else (m['name'], m['crc32']) in confirmed,
                     'in_candidate': None if candidate is None else (m['name'], m['crc32']) in candidate})
    boot = record['boot']['boot']
    return {'modules': rows,
            'draft': {'module': workbench['module'] or None, 'state': classify_draft(workbench),
                      'revision': workbench['revision'], 'bytes': workbench['length']},
            'boot': {'slot': boot['current'], 'confirmed': boot['confirmed'], 'state': boot['state'],
                     'trial_pending': boot['state'] != 'CLEAN',
                     'manifest_error': _manifest_error(record.get('manifest'), manifest)},
            'engine': record['resources']['resources'].get('engine'),
            'base': record['base']['base'],
            'data_stack_depth': record['stack']['data_stack']['depth_text'],
            'pending_module_stage': None,
            'open_handles': None if record.get('handles') is None else len(record['handles']),
            'last_error': None if not record.get('last_error') else
                          {k: record['last_error'][k] for k in ('serial', 'message', 'word', 'stale')},
            'live_tasks': None if record.get('tasks') is None else
                          [t['id'] for t in record['tasks'] if t['state'] in ('ready', 'sleeping')]}


class SnapshotClient:
    def _manifest(self, stack):
        # Firmware without BOOT-MANIFEST answers 'unknown word', and any Forth
        # error clears the data stack; only ask when there is nothing to lose.
        if stack['data_stack']['depth_text'].strip() != '0':
            return {'ok': False, 'error': 'skipped: data stack not empty'}
        try:
            result = self.boot_manifest()
        except IOError as error:
            return {'ok': False, 'error': str(error)}
        return result if result.get('ok') else {**result, 'error': result.get('error') or 'BOOT-MANIFEST failed'}

    def _base(self):
        result = self._query(BASE_QUERY)
        # A background task (an autorun app's heartbeat) may print a line into
        # the reply: the answer is the one line that is just a number.
        body = [line.strip() for line in self._body(result['output'], BASE_QUERY) if re.fullmatch(r'\s*\d{1,3}\s*', line)]
        if not result.get('ok') or len(body) != 1 or not 2 <= int(body[0]) <= 36:
            raise IOError('Malformed BASE response')
        result['base'] = int(body[0])
        return result

    def snapshot(self, previous_sources=False):
        if type(previous_sources) is not bool:
            raise ValueError('previous_sources must be a boolean')
        with self.lock:
            def need(result, label):
                if not result.get('ok'):
                    raise IOError(f'{label} query failed: {result.get("error") or result.get("output", "")}')
                return result
            status = need(self.wb_status(), 'WB-STATUS')
            first = status['workbench']
            record = {'format': FORMAT, 'captured_unix': round(time.time(), 3), 'status': status,
                      # No draft still yields the restore driver's source fields.
                      'source': need(self.wb_source(), 'WB-SOURCE') if first['draft'] else
                                {**status, 'source': '', 'revision': first['revision'], 'bytes': 0},
                      'stack': need(self.stacks(), '.S'), 'resources': need(self.resources(), 'RESOURCES'),
                      'modules': need(self.modules(), 'MODULES'), 'boot': need(self.boot_status(), 'BOOT-STATUS'),
                      'base': self._base(), 'module_sources': {}, 'previous_sources': {}}
            record['manifest'] = self._manifest(record['stack'])
            # Firmware before Stage 2 has no TASKS/HANDLES/LAST-ERROR; report absent.
            if hasattr(self, 'tasks'):
                listed = self.tasks()
                record['tasks'] = listed['tasks'] if listed.get('ok') else None
            if hasattr(self, 'handles'):
                listed = self.handles()
                record['handles'] = listed['handles'] if listed.get('ok') else None
            if hasattr(self, 'last_error'):
                listed = self.last_error()
                record['last_error'] = listed.get('last_error') if listed.get('ok') else None
            for m in record['modules']['modules']:
                if m['active']:
                    record['module_sources'][m['name']] = need(self.module_source(m['name']), 'MODULE-SOURCE')
                if previous_sources and m['previous']:
                    record['previous_sources'][m['name']] = need(self.module_source(m['name'], True), 'MODULE-SOURCE PREVIOUS')
            # A physical keyboard edit can land between queries even though host
            # tool calls are serialized; refuse a torn record instead of mixing.
            last = need(self.wb_status(), 'WB-STATUS')['workbench']
            if (last['revision'], last['catalog']) != (first['revision'], first['catalog']):
                raise IOError('Workbench or catalog changed during snapshot; retry when input is idle')
            for name, source in record['module_sources'].items():
                listed = next(m for m in record['modules']['modules'] if m['name'] == name)
                if source['crc32'] != listed['crc32']:
                    raise IOError(f'Module {name} source CRC does not match inventory')
            record['summary'] = summarize(record)
            record['ok'] = True
            return record
