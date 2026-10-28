class_name Hud
extends Control

signal score_changed(score: int)

var shown := 0

func _ready() -> void:
	Global.score = 5
	Global.add_points(3)
	score_changed.emit(Global.score)

func refresh() -> void:
	shown = Global.score
