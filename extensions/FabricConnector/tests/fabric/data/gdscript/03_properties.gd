extends Node

var health: int = 10:
	set(value):
		health = clampi(value, 0, 100)
		health_changed.emit(health)
	get:
		return health

var lives: int:
	set = set_lives, get = get_lives

var mana := 5:
	get:
		return mana * 2

var armor: float = 1.0:
	set(v):
		armor = v

var untyped:
	set(v):
		untyped = v
	get():
		return untyped

var shield: int:
	get = get_shield

var one_line: int: set = set_one_line, get = get_one_line

var tight := 0: get: return 7

signal health_changed(value)

func set_lives(v): lives = v
func get_lives(): return lives
func get_shield(): return 3
func set_one_line(v): one_line = v
func get_one_line(): return one_line
