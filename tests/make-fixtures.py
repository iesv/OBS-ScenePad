"""Small real OPC/Word fixtures; no python-docx dependency."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import json
import struct

folder = Path(__file__).parent / "fixtures"
folder.mkdir(exist_ok=True)
ns = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
parts = {
    '[Content_Types].xml': '''<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/><Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/></Types>''',
    '_rels/.rels': '''<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>''',
    'word/_rels/document.xml.rels': '''<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/></Relationships>''',
    'word/styles.xml': f'''<?xml version="1.0"?><w:styles xmlns:w="{ns}"><w:docDefaults><w:rPrDefault><w:rPr><w:sz w:val="24"/></w:rPr></w:rPrDefault></w:docDefaults><w:style w:type="paragraph" w:styleId="Normal"><w:name w:val="Normal"/></w:style><w:style w:type="paragraph" w:styleId="Title"><w:basedOn w:val="Normal"/><w:rPr><w:b/><w:sz w:val="44"/></w:rPr></w:style></w:styles>''',
    'word/numbering.xml': f'''<?xml version="1.0"?><w:numbering xmlns:w="{ns}"><w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:numFmt w:val="decimal"/></w:lvl></w:abstractNum><w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num></w:numbering>''',
    'word/document.xml': f'''<?xml version="1.0"?><w:document xmlns:w="{ns}"><w:body><w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr><w:r><w:t>Word import test</w:t></w:r></w:p><w:p><w:r><w:rPr><w:b/><w:color w:val="FF0000"/><w:highlight w:val="yellow"/></w:rPr><w:t>Red bold</w:t></w:r><w:r><w:t xml:space="preserve"> and normal 中文</w:t></w:r></w:p><w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>List item</w:t></w:r></w:p><w:p><w:r><w:drawing/></w:r></w:p></w:body></w:document>'''
}
def write_docx(filename):
    with ZipFile(folder / filename, 'w', ZIP_DEFLATED) as docx:
        for name, data in parts.items():
            docx.writestr(name, data.encode('utf-8'))
    print(folder / filename)

write_docx('basic.docx')
parts['word/document.xml'] = parts['word/document.xml'].replace('</w:body>', ''.join(
    f'<w:p><w:r><w:rPr><w:b/><w:sz w:val="24"/></w:rPr><w:t>Long Word paragraph {i} - 中文压力测试内容</w:t></w:r></w:p>'
    for i in range(2500)) + '</w:body>')
write_docx('long.docx')
parts['word/document.xml'] = f'<?xml version="1.0"?><w:document xmlns:w="{ns}"><w:body><w:p><w:r><w:t>' + 'W' * 250000 + '</w:t></w:r>' + ''.join(
    f'<w:r><w:rPr><w:sz w:val="{20 + i % 3 * 8}"/><w:b w:val="{i % 2}"/></w:rPr><w:t xml:space="preserve">' + 'alpha beta gamma ' * 5 + '</w:t></w:r>'
    for i in range(3000)) + '</w:p></w:body></w:document>'
write_docx('long-line.docx')

note = {'format': 'OBS-ScenePad', 'version': 1, 'sceneId': '11111111-1111-4111-8111-111111111111',
        'sceneName': 'HOME', 'html': '<p><b>External backup</b></p>'}
data = json.dumps(note, ensure_ascii=False).encode('utf-8')
with ZipFile(folder / 'backup-external.zip', 'w') as archive:
    archive.writestr('notes/HOME.json', data)
    note['sceneId'] = '22222222-2222-4222-8222-222222222222'
    archive.writestr('notes/开场.json', json.dumps(note, ensure_ascii=False).encode('utf-8'), compress_type=ZIP_DEFLATED)
with ZipFile(folder / 'backup-traversal.zip', 'w') as archive:
    archive.writestr('../escape.json', data)
with ZipFile(folder / 'backup-duplicate.zip', 'w') as archive:
    archive.writestr('one.json', data)
    archive.writestr('two.json', data)
original = (folder / 'backup-external.zip').read_bytes()
corrupt = bytearray(original)
corrupt[30 + struct.unpack_from('<H', corrupt, 26)[0]] ^= 1
(folder / 'backup-corrupt.zip').write_bytes(corrupt)
oversized = bytearray(original)
central = oversized.index(b'PK\x01\x02')
struct.pack_into('<I', oversized, central + 24, 65 * 1024 * 1024)
(folder / 'backup-oversized.zip').write_bytes(oversized)
(folder / 'backup-truncated.zip').write_bytes(original[:-8])
