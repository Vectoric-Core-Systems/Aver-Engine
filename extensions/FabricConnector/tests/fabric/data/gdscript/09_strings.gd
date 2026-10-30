extends Node

var plain = "double"
var single = 'single'
var quote_in = "it's \"quoted\""
var escapes = "tab\there\nnewline \\ backslash \u0041 \U01F600"
var raw = r"dir\path\n"
var raw_single = r'a\'b'
var raw_triple = r"""raw \n
second"""
var triple = """line one
line "two"
line three"""
var triple_single = '''it's
fine'''
var string_name = &"move_left"
var node_path = ^"Player/Camera:fov"
var joined = "a" + 'b'
var formatted = "%d items, %s" % [3, "ok"]
var cont = "abc\
def"
