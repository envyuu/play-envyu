#!/usr/bin/env python3
"""Save GitHub traffic stats as CSV before GitHub forgets them (it keeps only 14 days).

Usage: GH_TOKEN=... REPO=owner/name python3 tools/traffic.py OUTDIR

The token needs read access to the repo's traffic ("Administration: read" for a
fine-grained token). Running it again is safe: daily rows are updated in place,
snapshots are replaced for the same date.
"""
import csv
import datetime
import json
import os
import sys
import urllib.request

API = 'https://api.github.com/repos/'


def get(repo, path, token):
    req = urllib.request.Request(API + repo + path, headers={
        'Authorization': 'Bearer ' + token,
        'Accept': 'application/vnd.github+json',
        'User-Agent': 'traffic-stats',
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def read_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def write_rows(path, fields, rows):
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def merge_daily(path, views, clones):
    fields = ['date', 'views', 'views_unique', 'clones', 'clones_unique']
    rows = {r['date']: r for r in read_rows(path)}
    for key, data in (('views', views['views']), ('clones', clones['clones'])):
        for d in data:
            day = d['timestamp'][:10]
            row = rows.setdefault(day, {f: '0' for f in fields} | {'date': day})
            row[key] = str(d['count'])
            row[key + '_unique'] = str(d['uniques'])
    write_rows(path, fields, [rows[k] for k in sorted(rows)])


def replace_snapshot(path, fields, today, new_rows):
    rows = [r for r in read_rows(path) if r['date'] != today]
    rows += [{'date': today, **r} for r in new_rows]
    write_rows(path, fields, rows)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    token, repo = os.environ['GH_TOKEN'], os.environ['REPO']
    os.makedirs(out, exist_ok=True)
    today = datetime.date.today().isoformat()

    merge_daily(os.path.join(out, 'daily.csv'),
                get(repo, '/traffic/views', token), get(repo, '/traffic/clones', token))

    refs = get(repo, '/traffic/popular/referrers', token)
    replace_snapshot(os.path.join(out, 'referrers_14d.csv'), ['date', 'referrer', 'count', 'uniques'], today,
                     [{'referrer': r['referrer'], 'count': r['count'], 'uniques': r['uniques']} for r in refs])

    paths = get(repo, '/traffic/popular/paths', token)
    replace_snapshot(os.path.join(out, 'paths_14d.csv'), ['date', 'path', 'count', 'uniques'], today,
                     [{'path': p['path'], 'count': p['count'], 'uniques': p['uniques']} for p in paths])

    info = get(repo, '', token)
    releases = get(repo, '/releases?per_page=100', token)
    downloads = sum(a['download_count'] for r in releases for a in r.get('assets', []))
    replace_snapshot(os.path.join(out, 'repo.csv'),
                     ['date', 'stars', 'forks', 'watchers', 'open_issues', 'release_downloads'], today,
                     [{'stars': info['stargazers_count'], 'forks': info['forks_count'],
                       'watchers': info['subscribers_count'], 'open_issues': info['open_issues_count'],
                       'release_downloads': downloads}])
    print('saved traffic for', repo, 'to', out)


if __name__ == '__main__':
    main()
