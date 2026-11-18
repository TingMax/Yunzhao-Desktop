"""把当前桌面账号的多维表格快照写到 yunzhao-desktop.com 的 items。"""

import json
import os
import threading
import time
import uuid

import requests

HOST = 'https://yunzhao-desktop.com'
STAND_IN = 'a1000000-0000-4000-8000-000000000001'
_timer = None
_lock = threading.Lock()
_armed = False
_app = None


def _session_path():
    root = os.path.join(os.environ.get('LOCALAPPDATA') or '', 'Yunzhao')
    return os.path.join(root, 'session.bin')


def _blob(data):
    import ctypes
    from ctypes import wintypes

    class Blob(ctypes.Structure):
        _fields_ = [('cbData', wintypes.DWORD), ('pbData', ctypes.POINTER(ctypes.c_char))]

    buf = ctypes.create_string_buffer(data, len(data))
    return Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), buf


def read_session():
    from app.routes.auth import read_desktop_session
    data = read_desktop_session()
    if not data:
        return None
    if data.get('access_token') in ('local-admin', None, '') or str(data.get('id')) == STAND_IN:
        return None
    return data


def _write_session(data):
    import ctypes
    plain = json.dumps(data, ensure_ascii=False).encode('utf-8')
    incoming, _buf = _blob(plain)
    outgoing = type(incoming)()
    if not ctypes.windll.crypt32.CryptProtectData(ctypes.byref(incoming), 'Yunzhao', None, None, None, 1, ctypes.byref(outgoing)):
        return
    try:
        sealed = ctypes.string_at(outgoing.pbData, outgoing.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(outgoing.pbData)
    path = _session_path()
    tmp = path + '.tmp'
    with open(tmp, 'wb') as handle:
        handle.write(sealed)
    os.replace(tmp, path)


def _refresh(session):
    try:
        response = requests.post(
            HOST + '/api/auth/refresh',
            json={'refresh_token': session.get('refresh_token')},
            timeout=10,
        )
    except requests.RequestException:
        return None
    if response.status_code != 200:
        return None
    tokens = (response.json().get('data') or {}).get('tokens') or {}
    if not tokens.get('access_token'):
        return None
    session['access_token'] = tokens['access_token']
    if tokens.get('refresh_token'):
        session['refresh_token'] = tokens['refresh_token']
    session['expires_at'] = int(time.time()) + int(tokens.get('expires_in') or 86400)
    _write_session(session)
    return session


def _send(session, method, path, payload=None):
    def once():
        return requests.request(
            method,
            HOST + path,
            headers={'Authorization': 'Bearer ' + session['access_token']},
            json=payload,
            timeout=20,
        )

    try:
        response = once()
    except requests.RequestException:
        return None
    if response.status_code == 401 and _refresh(session):
        try:
            response = once()
        except requests.RequestException:
            return None
    return response


def _map_path(account_id):
    directory = os.path.join(os.environ.get('LOCALAPPDATA') or '', 'Yunzhao', 'users', account_id)
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, 'cloud-sheet.json')


def _snapshot(account_id):
    from app.models.base import Base
    from app.models.field import Field
    from app.models.record import Record
    from app.models.table import Table

    owner = uuid.UUID(str(account_id))
    bases = Base.query.filter_by(owner_id=owner).all()
    base_ids = [item.id for item in bases]
    tables = Table.query.filter(Table.base_id.in_(base_ids)).all() if base_ids else []
    table_ids = [item.id for item in tables]
    fields = Field.query.filter(Field.table_id.in_(table_ids)).all() if table_ids else []
    records = Record.query.filter(Record.table_id.in_(table_ids)).all() if table_ids else []
    return {
        '_src': 'smarttable:' + str(account_id),
        'bases': [item.to_dict() for item in bases],
        'tables': [item.to_dict() for item in tables],
        'fields': [item.to_dict() for item in fields],
        'records': [item.to_dict() for item in records],
    }


def _pull_if_empty(session):
    from datetime import datetime
    from app.extensions import db
    from app.models.base import Base
    from app.models.field import Field
    from app.models.record import Record
    from app.models.table import Table

    owner = uuid.UUID(str(session['id']))
    marker = os.path.join(os.path.dirname(_map_path(session['id'])), 'pull-sheet.once')
    replace = os.path.isfile(marker)
    if not replace and Base.query.filter_by(owner_id=owner).count() > 0:
        return False
    response = _send(session, 'GET', '/items?kind=sheet&limit=1')
    if response is None or response.status_code != 200:
        return False
    items = (response.json() or {}).get('items') or []
    row = items[0] if items else None
    body = (row or {}).get('payload') or {}
    source = str(body.get('_src') or '')
    if source and source != 'smarttable:' + str(session['id']):
        return False
    bases = body.get('bases') or []
    if not bases:
        return False
    if replace:
        from app.models.table import Table as TableModel
        from app.models.field import Field as FieldModel
        from app.models.record import Record as RecordModel
        owned = [item.id for item in Base.query.filter_by(owner_id=owner).all()]
        table_ids = [item.id for item in TableModel.query.filter(TableModel.base_id.in_(owned)).all()] if owned else []
        if table_ids:
            RecordModel.query.filter(RecordModel.table_id.in_(table_ids)).delete(synchronize_session=False)
            FieldModel.query.filter(FieldModel.table_id.in_(table_ids)).delete(synchronize_session=False)
            TableModel.query.filter(TableModel.id.in_(table_ids)).delete(synchronize_session=False)
        if owned:
            Base.query.filter(Base.id.in_(owned)).delete(synchronize_session=False)
        try:
            os.remove(marker)
        except OSError:
            pass

    def when(value):
        from datetime import timezone
        if isinstance(value, str) and value:
            try:
                return datetime.fromisoformat(value.replace('Z', '+00:00'))
            except ValueError:
                pass
        return datetime.now(timezone.utc)

    def uid(value):
        return uuid.UUID(str(value)) if value else None

    for item in bases:
        db.session.add(Base(
            id=uid(item.get('id')) or uuid.uuid4(),
            name=str(item.get('name') or '未命名')[:100],
            description=item.get('description'),
            owner_id=owner,
            icon=item.get('icon'),
            color=item.get('color') or '#6366F1',
            is_personal=bool(item.get('is_personal')),
            is_starred=bool(item.get('is_starred')),
            created_at=when(item.get('created_at')),
            updated_at=when(item.get('updated_at')),
        ))
    db.session.flush()
    tables = []
    for item in body.get('tables') or []:
        table = Table(
            id=uid(item.get('id')) or uuid.uuid4(),
            base_id=uid(item.get('base_id')),
            name=str(item.get('name') or '未命名')[:100],
            description=item.get('description'),
            order=int(item.get('order') or 0),
            folder_id=None,
            primary_field_id=None,
            created_at=when(item.get('created_at')),
            updated_at=when(item.get('updated_at')),
        )
        tables.append((table, item.get('primary_field_id')))
        db.session.add(table)
    db.session.flush()
    for item in body.get('fields') or []:
        raw = str(item.get('type') or 'text')
        if '.' in raw:
            raw = raw.split('.')[-1].lower()
        db.session.add(Field(
            id=uid(item.get('id')) or uuid.uuid4(),
            table_id=uid(item.get('table_id')),
            name=str(item.get('name') or '字段')[:100],
            type=raw[:50],
            description=item.get('description'),
            order=int(item.get('order') or 0),
            is_primary=bool(item.get('is_primary')),
            is_required=bool(item.get('is_required')),
            options=item.get('options') or {},
            config=item.get('config') or {},
            created_at=when(item.get('created_at')),
            updated_at=when(item.get('updated_at')),
        ))
    db.session.flush()
    for table, primary in tables:
        if primary:
            table.primary_field_id = uid(primary)
    for item in body.get('records') or []:
        db.session.add(Record(
            id=uid(item.get('id')) or uuid.uuid4(),
            table_id=uid(item.get('table_id')),
            values=item.get('values') or {},
            created_by=None,
            updated_by=None,
            created_at=when(item.get('created_at')),
            updated_at=when(item.get('updated_at')),
        ))
    db.session.commit()
    saved = row.get('id')
    if saved:
        with open(_map_path(session['id']), 'w', encoding='utf-8') as handle:
            json.dump({'id': int(saved)}, handle)
    return True


def sync_sheet():
    from app.extensions import db
    session = read_session()
    if not session:
        return
    try:
        _pull_if_empty(session)
    except Exception:
        db.session.rollback()
    try:
        payload = _snapshot(session['id'])
    except Exception:
        db.session.rollback()
        return
    if not payload.get('bases'):
        return
    encoded = json.dumps(payload, ensure_ascii=False, default=str)
    if len(encoded.encode('utf-8')) > 18 * 1024 * 1024:
        return
    body = json.loads(encoded)
    path = _map_path(session['id'])
    saved_id = 0
    if os.path.isfile(path):
        try:
            saved_id = int(json.load(open(path, encoding='utf-8')).get('id') or 0)
        except (OSError, ValueError, TypeError):
            saved_id = 0
    if saved_id:
        response = _send(session, 'PUT', f'/items/{saved_id}', {'payload': body})
    else:
        response = _send(session, 'POST', '/items', {'kind': 'sheet', 'payload': body})
    if response is None or response.status_code not in (200, 201):
        return
    try:
        new_id = int(response.json().get('id') or saved_id)
    except ValueError:
        return
    if new_id > 0:
        with open(path, 'w', encoding='utf-8') as handle:
            json.dump({'id': new_id}, handle)


def _run():
    if _app is None:
        return
    with _app.app_context():
        sync_sheet()


def schedule_sheet_sync():
    global _timer, _armed
    _armed = True
    with _lock:
        if _timer is not None:
            _timer.cancel()
        _timer = threading.Timer(3.0, _run)
        _timer.daemon = True
        _timer.start()


def register_cloud_sync(app):
    global _app
    _app = app
    from sqlalchemy import event
    from sqlalchemy.orm import Session

    def after_commit(_session):
        if _armed:
            schedule_sheet_sync()

    try:
        event.listen(Session, 'after_commit', after_commit)
    except Exception:
        return
