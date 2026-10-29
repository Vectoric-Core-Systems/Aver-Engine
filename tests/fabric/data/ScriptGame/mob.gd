class_name Mob
extends CharacterBody3D


func initialize(start_position, player_position):
	look_at_from_position(start_position, player_position, Vector3.UP)
