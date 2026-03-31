#!/usr/bin/env bash
# Fetch and parse the Perlmutter system timeline page as plain text
curl -s "https://docs.nersc.gov/systems/perlmutter/timeline/" | python3 -c "
import sys, html, re
content = sys.stdin.read()
content = re.sub(r'<script[^>]*>.*?</script>', '', content, flags=re.DOTALL)
content = re.sub(r'<style[^>]*>.*?</style>', '', content, flags=re.DOTALL)
content = re.sub(r'<nav[^>]*>.*?</nav>', '', content, flags=re.DOTALL)
content = re.sub(r'<header[^>]*>.*?</header>', '', content, flags=re.DOTALL)
content = re.sub(r'<footer[^>]*>.*?</footer>', '', content, flags=re.DOTALL)
content = re.sub(r'<br\s*/?>', '\n', content)
content = re.sub(r'<[hH][1-6][^>]*>', '\n\n### ', content)
content = re.sub(r'</[hH][1-6]>', '\n', content)
content = re.sub(r'<li[^>]*>', '\n- ', content)
content = re.sub(r'<p[^>]*>', '\n', content)
content = re.sub(r'<[^>]+>', ' ', content)
content = html.unescape(content)
content = re.sub(r'\n{3,}', '\n\n', content)
content = re.sub(r'[ \t]{2,}', ' ', content)
print(content.strip())
"
