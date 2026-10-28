@tool
class_name MainMenu
extends Control
## The title screen: buttons, a settings panel and a fading logo.

signal start_requested(level: String)
signal quit_requested
signal volume_changed(bus: StringName, value: float)

enum Panel { MAIN, SETTINGS, CREDITS }

const LEVELS := {
	"tutorial": "res://levels/tutorial.tscn",
	"city": "res://levels/city.tscn",
	"arena": "res://levels/arena.tscn",
}
const CLICK_SOUND = preload("res://audio/click.ogg")
const FADE_TIME: float = 0.4

@export var title: String = "My Game":
	set(value):
		title = value
		if is_node_ready():
			%Title.text = value
@export var default_panel: Panel = Panel.MAIN
@export_multiline var credits_text: String = ""
@export_file("*.tscn") var first_level: String
@export var accent: Color = Color("ff8844")

@onready var main_panel: Control = $Panels/Main
@onready var settings_panel: Control = $Panels/Settings
@onready var credits_panel: Control = $Panels/Credits
@onready var logo: TextureRect = %Logo
@onready var volume_slider: HSlider = $Panels/Settings/VBox/Volume
@onready var buttons: Array[Button] = [$Panels/Main/Start, $Panels/Main/Settings, $Panels/Main/Quit]

var current_panel: Panel = Panel.MAIN
var _tween: Tween
var _pending_level := ""

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	for b in buttons:
		b.pressed.connect(_on_button_pressed.bind(b.name))
		b.mouse_entered.connect(func(): b.modulate = accent)
		b.mouse_exited.connect(func(): b.modulate = Color.WHITE)
	volume_slider.value_changed.connect(func(v: float):
		volume_changed.emit(&"Master", v)
		AudioServer.set_bus_volume_db(0, linear_to_db(v))
	)
	show_panel(default_panel)
	logo.modulate.a = 0.0
	_tween = create_tween()
	_tween.tween_property(logo, "modulate:a", 1.0, FADE_TIME)

func show_panel(which: Panel) -> void:
	current_panel = which
	main_panel.visible = which == Panel.MAIN
	settings_panel.visible = which == Panel.SETTINGS
	credits_panel.visible = which == Panel.CREDITS
	match which:
		Panel.MAIN:
			buttons[0].grab_focus()
		Panel.SETTINGS:
			volume_slider.grab_focus()
		Panel.CREDITS:
			$Panels/Credits/Back.grab_focus()

func _on_button_pressed(button_name: StringName) -> void:
	play_click()
	match button_name:
		&"Start":
			start_game("tutorial")
		&"Settings":
			show_panel(Panel.SETTINGS)
		&"Quit":
			quit_requested.emit()
			get_tree().quit()
		_:
			push_warning("Unknown button: %s" % button_name)

func start_game(level_key: String) -> void:
	var path: String = LEVELS.get(level_key, first_level)
	if path.is_empty():
		push_error("No level for '%s'" % level_key)
		return
	_pending_level = path
	var fade := create_tween()
	fade.tween_property(self, "modulate:a", 0.0, FADE_TIME)
	await fade.finished
	start_requested.emit(path)
	get_tree().change_scene_to_file(path)

func play_click() -> void:
	var p := AudioStreamPlayer.new()
	p.stream = CLICK_SOUND
	add_child(p)
	p.play()
	p.finished.connect(p.queue_free)

func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel") and current_panel != Panel.MAIN:
		show_panel(Panel.MAIN)
		get_viewport().set_input_as_handled()
