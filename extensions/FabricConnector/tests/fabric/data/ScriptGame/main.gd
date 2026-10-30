extends Node3D

@export var mob_scene: PackedScene


func _ready():
	%Title.text = "ready"
	$Hud.hide()
