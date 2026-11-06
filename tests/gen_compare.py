#!/usr/bin/env python3
"""Random entries and queries for tests/compare-poedit.sh: python3
tests/gen_compare.py SEED > cases.txt. MIT, Copyright (c) 2026 AmiCradle Team."""
import random, sys
r = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
words = ("open save close file files folder the a to of document changes error disk "
         "print preview new window tab font size colour color settings preferences "
         "don't can't it's user's copy paste cut undo redo find replace next previous "
         "Ünïcode čeština žluťoučký kůň úpěl ďábelské ódy 2026 v1 x86").split()
langs = [("en", "cs"), ("en", "cs_CZ"), ("en", "de"), ("en", "pt_BR"), ("en", "pt"), ("de", "cs")]
def phrase(n):
    return " ".join(r.choice(words) for _ in range(n)).capitalize() + r.choice(["", "", ".", "...", "?", ":"])
ents = []
for i in range(300):
    s, l = r.choice(langs)
    src = phrase(r.randint(1, 9))
    ents.append((s, l, src))
    print("insert\t%s\t%s\t%s\t%s\t%d" % (s, l, src, "T%d %s" % (i % 120, src[::-1]), 1600000000 + i))
    if r.random() < 0.05:
        print("commit")
print("commit")
for i in range(250):
    s, l, src = r.choice(ents)
    ql = r.choice([l, l.split("_")[0], "cs", "pt_BR", "de"])
    k = r.random()
    if k < 0.3:
        q = src
    elif k < 0.6:
        ws = src.split(); 
        if len(ws) > 1: ws.pop(r.randrange(len(ws)))
        q = " ".join(ws)
    elif k < 0.8:
        ws = src.split(); ws.insert(r.randrange(len(ws) + 1), r.choice(words)); q = " ".join(ws)
    else:
        q = phrase(r.randint(1, 6))
    print("search\t%s\t%s\t%s" % (s, ql, q))
    if r.random() < 0.15:
        print("sub\t%s\t%s\t%s" % (s, ql, r.choice(src.split())))
