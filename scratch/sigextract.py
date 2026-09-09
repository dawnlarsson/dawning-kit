import re

text = open('src/build/build.c').read()
start = text.index('{"kernel_signature",')
# The entry ends at the "}," that closes it: find the last string literal line
# before a line that is exactly '},' at the settings indentation.
end = text.index('},', text.index('-----END PGP SIGNATURE-----', start))
body = text[start:end]

pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
# Drop the setting's own name.
assert pieces[0] == 'kernel_signature', pieces[0]
value = ''.join(pieces[1:])
value = value.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')

open('scratch/sig.new', 'w').write(value)
print('wrote %d bytes' % len(value))
