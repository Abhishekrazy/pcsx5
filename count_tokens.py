import re
data = open('Games/PPSA02929-app0/data.js', 'r', encoding='utf-8-sig').read()[:65536]
tokens = re.findall(r'\"[^\"]*\"|\d+|true|false|null|[\[\]\{\}\:\,]', data)
print('Tokens:', len(tokens))
