#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the finite public depth/stencil obligation denominator offline."""
import copy
import hashlib
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[5]
MATRIX = ROOT / 'docs/hardware/r3v-public-depth-stencil-obligations.json'
REVISION = '57692834480e19201bb9efe9a67f7a65876b7a32'
REGISTRY_SHA256 = '80e7394d0e787d6ec78b67aa324add6f96129fdd042ba640cc336a5481a208ee'
REQUIRED = set('''format-minimums limits image-create memory-binding aspect-views
    depth-state stencil-state dynamic-state depth-bounds depth-load-store
    stencil-load-store attachment-clear image-clear image-copy buffer-copy-depth
    buffer-copy-stencil blit-restrictions layouts visibility subpass-dependencies
    queue-lifetime shader-early-tests shader-late-tests'''.split())
CHAPTERS = {'features', 'resources', 'renderpass', 'clears', 'copies',
            'synchronization', 'fragops'}

CHAPTER_HASHES = {
    'features': '591a5ab0e1be5aa344e138ecbb48e9e8529ab37474637827a1a60b4ac91cabba',
    'resources': 'fc7dc6245cc0c16f006b851f1c850acc8a7b010d8b54d04fb771b3faf8ccd42c',
    'renderpass': 'eadc64f118aec8522249643c605a6fe9933b94bfcc3522dfe45d6ba20edc62cf',
    'clears': '1ba04c57a175c26266df550696620f63a8bd88601fde907e0f45a0b2e224b15c',
    'copies': 'c71f5cdfe507074591d4477109870159c6871ce854015fe88b190ea9ec87321d',
    'synchronization': '5be475133419fdc81f50c97f0d0110ad8db5d905ce322e74860f3096964a9cf7',
    'fragops': '3f67baa6f93f0fdbf550e5595f41b43b95e96bbd32c283ea792d7bad0e0aac74',
}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key')
        result[key] = value
    return result


def validate(document):
    require(document['schema'] == 'r3v-public-depth-stencil-obligations/2', 'schema')
    scope = document['scope']
    require(scope == {'api':'Vulkan 1.0','extensions':[],'maintenance1':False,
                     'experimental_image':{'silicon':'RS485M','pci_device':'1002:5974',
                     'subsystem':'1028:022a','format':'VK_FORMAT_D24_UNORM_S8_UINT',
                     'extent':[64,64],'samples':1}, 'capability_advertisement':True},
            'bounded core API scope')
    registry = document['authority']['registry']
    require(registry == {'path':'src/vulkan/registry/vk.xml','header_version':354,
                         'sha256':REGISTRY_SHA256}, 'registry pin')
    registry_bytes = (ROOT / registry['path']).read_bytes()
    require(hashlib.sha256(registry_bytes).hexdigest() == REGISTRY_SHA256,
            'registry content drift')
    spec = document['authority']['spec']
    require(spec['revision'] == REVISION and spec['tag'] == 'v1.0.68-core' and
            spec['tag_object'] == '88ec766f6aac618f1858fd80758d7a8aad458a2c' and
            spec['repository'] == 'https://github.com/KhronosGroup/Vulkan-Docs', 'spec pin')
    require(set(spec['chapters']) == CHAPTERS, 'chapter denominator')
    for chapter, source in spec['chapters'].items():
        path = 'doc/specs/vulkan/chapters/' + chapter + '.txt'
        require(source['path'] == path and source['url'] ==
                'https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/' + REVISION + '/' + path,
                'immutable chapter locator')
        require(source['sha256'] == CHAPTER_HASHES[chapter], 'chapter digest')
    rows = document['rows']
    require(len(rows) == len(REQUIRED) and {row['id'] for row in rows} == REQUIRED,
            'required obligation rows')
    for row in rows:
        require(set(row) == {'id','authority','anchor','registry_symbol','requirement',
                            'falsifier','public_status','completion_evidence','experimental_status'},
                'row fields')
        require(row['authority'] in CHAPTERS and row['anchor'] and
                row['registry_symbol'].encode() in registry_bytes, 'row authority')
        require(row['public_status'] in {'bounded', 'residual'},
                'public status vocabulary')
        if row['public_status'] == 'bounded':
            require(row['completion_evidence'], 'bounded completion evidence')
            require(row['experimental_status'].startswith('bounded public route; '),
                    'bounded evidence class separation')
        else:
            require(row['completion_evidence'] == [], 'residual completion evidence')
            require(row['experimental_status'].startswith('residual public obligation; '),
                    'residual evidence class separation')
        require(all(type(row[key]) is str and len(row[key]) >= 20
                    for key in ('requirement','falsifier')), 'operational obligation')
    return len(rows)


class MatrixTests(unittest.TestCase):
    def setUp(self):
        self.document = json.loads(MATRIX.read_text(), object_pairs_hook=unique_pairs)

    def test_positive(self):
        self.assertEqual(validate(self.document), 23)

    def test_bounded_surface_partition(self):
        validate(self.document)
        statuses = {row['id']: row['public_status']
                    for row in self.document['rows']}
        self.assertEqual(sum(status == 'bounded' for status in statuses.values()), 14)
        self.assertEqual(sum(status == 'residual' for status in statuses.values()), 9)
        self.assertEqual(statuses['image-clear'], 'bounded')
        self.assertEqual(statuses['stencil-state'], 'bounded')
        self.assertEqual(statuses['dynamic-state'], 'residual')

    def test_every_missing_row_refuses(self):
        for index in range(len(self.document['rows'])):
            changed = copy.deepcopy(self.document)
            changed['rows'].pop(index)
            with self.assertRaisesRegex(ValueError, 'required obligation rows'):
                validate(changed)

    def test_unknown_status_refuses(self):
        for row in self.document['rows']:
            previous = row['public_status']
            row['public_status'] = 'complete'
            with self.assertRaisesRegex(ValueError, 'public status vocabulary'):
                validate(self.document)
            row['public_status'] = previous
        bounded_row = next(row for row in self.document['rows']
                           if row['public_status'] == 'bounded')
        bounded_row['completion_evidence'] = []
        with self.assertRaisesRegex(ValueError, 'bounded completion evidence'):
            validate(self.document)

    def test_scope_and_provenance_refuse(self):
        self.document['scope']['maintenance1'] = True
        with self.assertRaisesRegex(ValueError, 'bounded core API scope'):
            validate(self.document)
        self.document['scope']['maintenance1'] = False
        self.document['authority']['spec']['revision'] = 'main'
        with self.assertRaisesRegex(ValueError, 'spec pin'):
            validate(self.document)

    def test_residual_evidence_refuses(self):
        residual = next(row for row in self.document['rows']
                        if row['public_status'] == 'residual')
        residual['completion_evidence'] = ['unverified execution']
        with self.assertRaisesRegex(ValueError, 'residual completion evidence'):
            validate(self.document)

    def test_duplicate_key_refuses(self):
        with self.assertRaisesRegex(ValueError, 'duplicate JSON key'):
            json.loads('{"status":1,"status":2}', object_pairs_hook=unique_pairs)


if __name__ == '__main__':
    unittest.main()
