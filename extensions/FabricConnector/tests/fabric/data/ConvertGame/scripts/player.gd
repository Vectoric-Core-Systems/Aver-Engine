class_name Player
extends CharacterBody3D

@export var speed := 5.0
@export var label := "Player"
@export var target: Node3D

signal hit(damage)

func _on_trigger_body_entered(body, a, b):
	pass
