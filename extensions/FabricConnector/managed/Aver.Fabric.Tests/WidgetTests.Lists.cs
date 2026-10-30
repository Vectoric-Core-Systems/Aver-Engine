// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 U2 tests, second half: menus, popups and dialogs, the text fields, the list and tree controls, tabs, the layout widgets, the colour picker, scroll bars.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;
using Range = Aver.Fabric.Range;

static partial class WidgetTests
{
    private static List<string> WatchMenu(PopupMenu m)
    {
        var log = new List<string>();
        m.IdPressed += id => log.Add("id:" + id);
        m.IndexPressed += i => log.Add("index:" + i);
        return log;
    }

    private static void PopupMenuBehaviour()
    {
        var pm = new PopupMenu { Name = "Menu" };
        pm.AddItem("Open", 1);
        pm.AddItem("Save", 2);
        pm.AddSeparator();
        pm.AddCheckItem("Auto", 3);
        pm.AddRadioCheckItem("R1", 4);
        pm.AddItem("Off", 5);
        pm.SetItemDisabled(5, true);
        Root.AddChild(pm);
        List<string> log = WatchMenu(pm);
        T.Check("a menu starts hidden, with its items and their ids", !pm.Visible && pm.GetItemCount() == 6 && pm.GetItemId(3) == 3 && pm.GetItemIndex(2) == 1 && pm.GetItemText(0) == "Open");
        T.Check("items know their kind", pm.IsItemSeparator(2) && pm.IsItemCheckable(3) && !pm.IsItemRadioCheckable(3) && pm.IsItemRadioCheckable(4) && pm.IsItemDisabled(5) && !pm.IsItemChecked(3));
        pm.Popup(new Rect2I(10, 10, 0, 0));
        T.Check("popup() shows it at its content size: 8 margins + 2 + the 20 px check column + 32 text + 2", pm.IsPopupShown && T.Near(pm.Size.X, 64f), $"{pm.Size}");
        T.Check("the items are under the pointer by row (4 px margin, 23.2 px rows)", pm.GetItemAt(new Vector2(20, 10)) == 0 && pm.GetItemAt(new Vector2(20, 30)) == 1 && pm.GetItemAt(new Vector2(-5, 10)) == -1);

        Tap(30, 10 + 4 + 23.2f + 11f);
        T.Check("clicking an item emits id_pressed then index_pressed and hides the menu", log.SequenceEqual(new[] { "id:2", "index:1" }) && !pm.Visible, string.Join(",", log));
        log.Clear();
        pm.ActivateItem(3);
        T.Check("a check item toggles on activation", pm.IsItemChecked(3) && log.SequenceEqual(new[] { "id:3", "index:3" }));
        pm.ActivateItem(3);
        T.Check("and back", !pm.IsItemChecked(3));
        pm.ActivateItem(3);
        pm.ActivateItem(4);
        T.Check("a radio item is checked by activation but the others stay (the script unchecks them)", pm.IsItemChecked(3) && pm.IsItemChecked(4));
        log.Clear();
        pm.ActivateItem(5);
        pm.ActivateItem(2);
        T.Check("a disabled item and a separator do nothing", log.Count == 0);

        pm.HideOnItemSelection = false;
        pm.Popup(new Rect2I(10, 10, 0, 0));
        pm.ActivateItem(0);
        T.Check("hide_on_item_selection off keeps the menu open", pm.IsPopupShown);
        pm.HideOnCheckableItemSelection = true;
        pm.ActivateItem(3);
        T.Check("hide_on_checkable_item_selection still closes it for a check item", !pm.IsPopupShown);
        pm.HideOnItemSelection = true;

        log.Clear();
        pm.AddMultistateItem("Mode", 3, 0, 20);
        int mode = pm.GetItemCount() - 1;
        pm.ActivateItem(mode);
        pm.ActivateItem(mode);
        pm.ActivateItem(mode);
        T.Check("a multistate item cycles through its states and does not hide the menu by default", pm.GetItemMultistate(mode) == 0 && log.Count(l => l == "id:20") == 3);

        var sc = new Shortcut { Events = new Collections.Array { (Variant)new InputEventKey { Keycode = Key.F2 } } };
        pm.AddShortcut(sc, 7);
        pm.AddItem("Quit", 8, Key.Q);
        log.Clear();
        T.Check("a shortcut item is found by its event", pm.ActivateItemByEvent(new InputEventKey { Keycode = Key.F2, Pressed = true }) && log.SequenceEqual(new[] { "id:7", "index:" + (pm.GetItemCount() - 2) }), string.Join(",", log));
        log.Clear();
        T.Check("an accelerator key too", pm.ActivateItemByEvent(new InputEventKey { Keycode = Key.Q, Pressed = true }) && log.Contains("id:8"));
        T.Check("and a key nothing owns is refused", !pm.ActivateItemByEvent(new InputEventKey { Keycode = Key.Z, Pressed = true }));

        // keyboard
        pm.Popup(new Rect2I(10, 10, 0, 0));
        log.Clear();
        Press(Key.Down);
        Press(Key.Down);
        T.Check("Down moves the focus over the enabled items (Open, then Save)", pm.GetFocusedItem() == 1);
        Press(Key.Enter);
        T.Check("accept activates the focused item and hides", log.Contains("id:2") && !pm.IsPopupShown);
        pm.Popup(new Rect2I(10, 10, 0, 0));
        Type("a");
        T.Check("typing a letter jumps to the item that starts with it", pm.GetItemText(pm.GetFocusedItem()) == "Auto");
        Press(Key.Escape);
        T.Check("Escape closes the menu", !pm.IsPopupShown);
        pm.Popup(new Rect2I(10, 10, 0, 0));
        Tap(600, 600);
        T.Check("a click outside closes it and is used up", !pm.IsPopupShown);

        // submenus
        var main = new PopupMenu { Name = "Main" };
        var sub = new PopupMenu { Name = "Sub" };
        sub.AddItem("Deep", 10);
        main.AddItem("Top", 1);
        main.AddSubmenuItem("More", "Sub", 9);
        main.AddChild(sub);
        Root.AddChild(main);
        List<string> subLog = WatchMenu(sub);
        List<string> mainLog = WatchMenu(main);
        main.Popup(new Rect2I(100, 100, 0, 0));
        Move(110, 100 + 4 + 23.2f + 11f);
        T.Check("hovering a submenu item opens the submenu beside it", sub.IsPopupShown && T.Near(sub.GlobalPosition.X, 100 + main.Size.X) && main.GetItemSubmenu(1) == "Sub");
        sub.ActivateItem(0);
        T.Check("choosing in the submenu reports there and closes the whole chain", subLog.SequenceEqual(new[] { "id:10", "index:0" }) && mainLog.Count == 0 && !sub.IsPopupShown && !main.IsPopupShown);

        // editing
        int changed = 0;
        pm.MenuChanged += () => changed++;
        pm.SetItemText(0, "Open...");
        pm.RemoveItem(pm.GetItemCount() - 1);
        T.Check("changing items emits menu_changed", changed >= 2 && pm.GetItemText(0) == "Open...");
        pm.Clear();
        T.Check("clear empties it", pm.GetItemCount() == 0);
    }

    private static void OptionAndMenuButtons()
    {
        var ob = new OptionButton();
        ob.AddItem("Easy", 10);
        ob.AddItem("Normal", 20);
        ob.AddItem("Hard", 30);
        Root.AddChild(ob);
        ob.Position = new Vector2(20, 20);
        ob.Size = new Vector2(100, 30);
        Settle();
        T.Check("the first item is selected as soon as there is one", ob.Selected == 0 && ob.Text == "Easy" && ob.GetSelectedId() == 10 && ob.ItemCount == 3);
        T.Check("the button is as wide as its longest item: 32 + 16 + (48 - 32) + 16 for the arrow", T.Near(ob.GetCombinedMinimumSize().X, 80f), $"{ob.GetCombinedMinimumSize()}");
        var log = new List<string>();
        ob.ItemSelected += i => log.Add("selected:" + i);
        ob.Select(2);
        T.Check("select() shows the item without a signal", ob.Text == "Hard" && ob.GetSelectedId() == 30 && log.Count == 0);
        ob.GetPopup().ActivateItem(1);
        T.Check("choosing from the popup selects it and emits item_selected with the index", ob.Selected == 1 && ob.Text == "Normal" && log.SequenceEqual(new[] { "selected:1" }), string.Join(",", log));
        ob.GetPopup().ActivateItem(1);
        T.Check("choosing the same item again emits nothing", log.Count == 1);
        ob.AllowReselect = true;
        ob.GetPopup().ActivateItem(1);
        T.Check("unless allow_reselect", log.Count == 2);
        Tap(30, 30);
        T.Check("a press on the button opens the popup below it, at least as wide as the button", ob.GetPopup().IsPopupShown && T.Near(ob.GetPopup().GlobalPosition.Y, 50f) && ob.GetPopup().Size.X >= 100f, $"{ob.GetPopup().GlobalPosition} {ob.GetPopup().Size}");
        Tap(40, 50 + 4 + 2 * 23.2f + 11f);
        T.Check("clicking the third row picks Hard", ob.Selected == 2 && !ob.GetPopup().IsPopupShown && log.Last() == "selected:2", string.Join(",", log));
        ob.SetItemDisabled(0, true);
        ob.GetPopup().ActivateItem(0);
        T.Check("a disabled item cannot be chosen", ob.Selected == 2 && ob.IsItemDisabled(0));
        ob.AddSeparator();
        T.Check("separators count as items but cannot be chosen", ob.ItemCount == 4 && ob.IsItemSeparator(3) && ob.GetSelectableItem() == 1 && ob.GetSelectableItem(true) == 2);
        ob.SetItemMetadata(2, "meta");
        T.Check("metadata rides on the item", ob.GetSelectedMetadata().AsString() == "meta");
        ob.RemoveItem(2);
        T.Check("removing the selected item clears the selection and the text", ob.Selected == -1 && ob.Text == "");
        ob.Clear();
        T.Check("clear removes everything", ob.ItemCount == 0 && !ob.GetPopup().IsPopupShown);
        Tap(30, 30);
        T.Check("a button with no items opens nothing", !ob.GetPopup().IsPopupShown);

        var mb = new MenuButton { Text = "File" };
        mb.GetPopup().AddItem("New", 1);
        mb.GetPopup().AddCheckItem("Auto", 2);
        Root.AddChild(mb);
        mb.Position = new Vector2(300, 20);
        mb.Size = new Vector2(80, 30);
        Settle();
        var mlog = WatchMenu(mb.GetPopup());
        int about = 0;
        mb.AboutToPopup += () => about++;
        Tap(310, 30);
        T.Check("a press opens the menu under the button and says about_to_popup", mb.GetPopup().IsPopupShown && about == 1 && T.Near(mb.GetPopup().GlobalPosition.Y, 50f) && mb.ButtonPressed);
        Tap(320, 50 + 4 + 23.2f + 11f);
        T.Check("choosing an item reports on the menu's popup and releases the button", mlog.SequenceEqual(new[] { "id:2", "index:1" }) && mb.GetPopup().IsItemChecked(1) && !mb.ButtonPressed, string.Join(",", mlog));
        T.Check("the button has no focus of its own", mb.FocusMode == Control.FocusModeEnum.None);
    }

    private static void PopupsAndDialogs()
    {
        var dlg = new AcceptDialog { DialogText = "Saved!", Title = "Info", Name = "Dlg" };
        Root.AddChild(dlg);
        var log = new List<string>();
        dlg.Confirmed += () => log.Add("confirmed");
        dlg.Canceled += () => log.Add("canceled");
        dlg.AboutToPopup += () => log.Add("about");
        dlg.PopupHide += () => log.Add("hide");
        T.Check("a dialog starts hidden and has an OK button and the message", !dlg.Visible && dlg.GetOkButton().Text == "OK" && dlg.GetLabel().Text == "Saved!" && dlg.DialogHideOnOk);
        dlg.PopupCentered();
        T.Check("popup_centered centres it in the window and says about_to_popup first", dlg.IsPopupShown && log[0] == "about" && T.Near(dlg.Position.X + dlg.Size.X / 2f, 640f, 1f) && T.Near(dlg.Position.Y + dlg.Size.Y / 2f, 360f, 1f), $"{dlg.GetRect()}");
        T.Check("it is at least as big as the message and button", dlg.Size.X >= 40f && dlg.Size.Y >= 19.2f + 27.2f);
        Vector2 ok = dlg.GetOkButton().GetGlobalRect().GetCenter();
        Tap(ok.X, ok.Y);
        T.Check("OK confirms and hides (hide first, as in Godot)", log.SequenceEqual(new[] { "about", "hide", "confirmed" }) && !dlg.IsPopupShown, string.Join(",", log));
        log.Clear();
        dlg.PopupCentered();
        Press(Key.Escape);
        T.Check("Escape cancels and hides", log.Contains("canceled") && !dlg.IsPopupShown, string.Join(",", log));
        log.Clear();
        var below = MakeButton("Below", 5, 5, 30, 20);
        List<string> belowLog = Watch(below);
        dlg.GetOkButton().Text = "Fine";
        dlg.PopupCentered(new Vector2I(300, 200));
        T.Check("a minimum size is honoured", dlg.Size.X >= 300 && dlg.Size.Y >= 200);
        Tap(10, 10);
        T.Check("a click outside an exclusive dialog does not reach the scene or close it", belowLog.Count == 0 && dlg.IsPopupShown);
        dlg.DialogHideOnOk = false;
        ok = dlg.GetOkButton().GetGlobalRect().GetCenter();
        Tap(ok.X, ok.Y);
        T.Check("dialog_hide_on_ok off keeps it open", dlg.IsPopupShown && log.Contains("confirmed"));
        dlg.Hide();

        var custom = dlg.AddButton("Extra", false, "more");
        dlg.AddButton("Plain");
        var actions = new List<string>();
        dlg.CustomAction += a => actions.Add(a.ToString());
        dlg.PopupCentered();
        Vector2 c = custom.GetGlobalRect().GetCenter();
        Tap(c.X, c.Y);
        T.Check("a custom button emits custom_action with its action and leaves the dialog open", actions.SequenceEqual(new[] { "more" }) && dlg.IsPopupShown);
        dlg.RemoveButton(custom);
        T.Check("remove_button takes it away", custom.GetParent() != dlg);
        dlg.Hide();

        var confirm = new ConfirmationDialog { DialogText = "Sure?" };
        Root.AddChild(confirm);
        var clog = new List<string>();
        confirm.Confirmed += () => clog.Add("confirmed");
        confirm.Canceled += () => clog.Add("canceled");
        confirm.PopupCentered();
        T.Check("a confirmation dialog has a cancel button too", confirm.GetCancelButton().Text == "Cancel" && confirm.Title == "Please Confirm...");
        Vector2 cc = confirm.GetCancelButton().GetGlobalRect().GetCenter();
        Tap(cc.X, cc.Y);
        T.Check("cancel emits canceled and hides", clog.SequenceEqual(new[] { "canceled" }) && !confirm.IsPopupShown, string.Join(",", clog));
        confirm.Popup(new Rect2I(50, 50, 300, 120));
        T.Check("popup(rect) places it exactly", IsRect(confirm.GetRect(), 50, 50, 300, 120), $"{confirm.GetRect()}");
        confirm.PopupCenteredRatio(0.5f);
        T.Check("popup_centered_ratio takes that share of the window", T.Near(confirm.Size.X, 640f) && T.Near(confirm.Size.Y, 360f), $"{confirm.Size}");
        confirm.Hide();

        var panel = new PopupPanel();
        var inside = new Label { Text = "Hello" };
        panel.AddChild(inside);
        Root.AddChild(panel);
        Settle();
        panel.Popup();
        Settle();
        T.Check("a popup panel is its child's size plus the panel's margins", Is(panel.Size, 48, 27.2f) && Is(inside.Size, 40, 19.2f) && Is(inside.Position, 4, 4), $"{panel.Size} {inside.Size} {inside.Position}");
        panel.Hide();
        T.Check("hiding it takes it off the popup list", !panel.IsPopupShown && !Gui.Popups.Contains(panel));
    }

    // ---- LineEdit ----

    private static LineEdit MakeLine(string text = "")
    {
        var le = new LineEdit { Text = text };
        Root.AddChild(le);
        le.Position = new Vector2(10, 10);
        le.Size = new Vector2(200, 30);
        Settle();
        le.GrabFocus();
        return le;
    }

    private static void LineEditBehaviour()
    {
        LineEdit le = MakeLine();
        var changed = new List<string>();
        var submitted = new List<string>();
        var rejected = new List<string>();
        le.TextChanged += t => changed.Add(t);
        le.TextSubmitted += t => submitted.Add(t);
        le.TextChangeRejected += t => rejected.Add(t);
        T.Check("a line edit is four 'M's wide plus the style, a line high plus the style", Is(le.GetCombinedMinimumSize(), 40, 27.2f), $"{le.GetCombinedMinimumSize()}");
        Type("abc");
        T.Check("typing inserts at the caret and emits text_changed with the new text each time", le.Text == "abc" && changed.SequenceEqual(new[] { "a", "ab", "abc" }) && le.CaretColumn == 3, string.Join(",", changed));
        Press(Key.Left);
        Type("X");
        T.Check("the caret moves with the arrow keys and typing goes in front of it", le.Text == "abXc" && le.CaretColumn == 3);
        Press(Key.Backspace);
        T.Check("backspace deletes before the caret", le.Text == "abc" && le.CaretColumn == 2);
        Press(Key.Delete);
        T.Check("delete deletes after it", le.Text == "ab" && le.CaretColumn == 2);
        Press(Key.Home);
        Key_(Key.Right, true, shift: true);
        Key_(Key.Right, true, shift: true);
        T.Check("shift and the arrows select", le.HasSelection() && le.GetSelectedText() == "ab" && le.GetSelectionFromColumn() == 0 && le.GetSelectionToColumn() == 2);
        Type("Z");
        T.Check("typing replaces the selection", le.Text == "Z" && !le.HasSelection());
        int before = changed.Count;
        le.Text = "hello world foo";
        T.Check("setting the text from code emits no text_changed", changed.Count == before && le.Text == "hello world foo");
        Key_(Key.A, true, ctrl: true);
        T.Check("Ctrl+A selects all", le.GetSelectedText() == "hello world foo");
        Key_(Key.C, true, ctrl: true);
        Press(Key.End);
        Key_(Key.V, true, ctrl: true);
        T.Check("Ctrl+C and Ctrl+V copy and paste through the clipboard", le.Text == "hello world foohello world foo");
        Key_(Key.Z, true, ctrl: true);
        T.Check("Ctrl+Z undoes the paste", le.Text == "hello world foo");
        Key_(Key.Y, true, ctrl: true);
        T.Check("Ctrl+Y redoes it", le.Text == "hello world foohello world foo");
        Key_(Key.Z, true, ctrl: true);
        Key_(Key.A, true, ctrl: true);
        Key_(Key.X, true, ctrl: true);
        T.Check("Ctrl+X cuts", le.Text == "");
        Key_(Key.V, true, ctrl: true);
        T.Check("and the cut text is pasteable", le.Text == "hello world foo");
        le.Text = "one two three";
        le.CaretColumn = 13;
        Key_(Key.Left, true, ctrl: true);
        T.Check("Ctrl+Left jumps a word back", le.CaretColumn == 8);
        Key_(Key.Backspace, true, ctrl: true);
        T.Check("Ctrl+Backspace deletes a word", le.Text == "one three" && le.CaretColumn == 4, $"{le.Text}");
        Press(Key.Enter);
        T.Check("Enter emits text_submitted with the text", submitted.SequenceEqual(new[] { "one three" }));

        // max length
        var limited = new LineEdit { MaxLength = 3 };
        Root.AddChild(limited);
        limited.Position = new Vector2(10, 100);
        limited.Size = new Vector2(100, 30);
        Settle();
        limited.GrabFocus();
        limited.TextChangeRejected += t => rejected.Add(t);
        Type("abcdef");
        T.Check("max_length stops the text and reports what did not fit", limited.Text == "abc" && string.Join("", rejected) == "def", $"{limited.Text} {string.Join("", rejected)}");
        limited.Text = "toolongtext";
        T.Check("setting a longer text cuts it", limited.Text == "too");

        // secret, placeholder, alignment
        var pw = new LineEdit { Text = "pw", Secret = true };
        Root.AddChild(pw);
        T.Check("a secret field draws the secret character, not the text", Texts(pw)[0].Text == "••" && pw.Text == "pw");
        pw.SecretCharacter = "*";
        T.Check("and the character can be chosen", Texts(pw)[0].Text == "**");
        var ph = new LineEdit { PlaceholderText = "name" };
        Root.AddChild(ph);
        CanvasItem.DrawCommand pc = Texts(ph)[0];
        T.Check("an empty field draws its placeholder in the placeholder colour", pc.Text == "name" && T.Near(pc.Color.A, 0.6f, 0.01f));
        var al = MakeLine("abc");
        al.Alignment = HorizontalAlignment.Right;
        T.Check("right alignment puts the text at the right end of the content: 200 - 4 - 24", T.Near(Texts(al)[0].Rect.Position.X, 172f), $"{Texts(al)[0].Rect.Position.X}");
        al.Editable = false;
        Type("q");
        T.Check("an uneditable field refuses typing", al.Text == "abc");
        T.Check("and draws in the uneditable colour", T.Near(Texts(al)[0].Color.A, 0.5f, 0.01f));

        // mouse
        LineEdit mouse = MakeLine("hello");
        mouse.Position = new Vector2(10, 300);
        Settle();
        Tap(10 + 4 + 18, 315);
        T.Check("a click puts the caret at the nearest character gap: 18 px is two characters in", mouse.CaretColumn == 2, $"{mouse.CaretColumn}");
        Click(10 + 4 + 2, 315, true);
        Move(10 + 4 + 33, 315, 31, 0);
        Click(10 + 4 + 33, 315, false);
        T.Check("dragging selects", mouse.GetSelectedText() == "hell", $"'{mouse.GetSelectedText()}'");
        mouse.Text = "ab cd";
        Click(10 + 4 + 36, 315, true, MouseButton.Left, dbl: true);
        Click(10 + 4 + 36, 315, false);
        T.Check("a double click selects the word", mouse.GetSelectedText() == "cd", $"'{mouse.GetSelectedText()}'");
        mouse.Select(1, 3);
        T.Check("select(from, to) selects a range", mouse.GetSelectedText() == "b ");
        mouse.Deselect();
        mouse.InsertTextAtCaret("!");
        T.Check("insert_text_at_caret types", mouse.Text.Contains('!'));
        mouse.DeleteText(0, 2);
        T.Check("delete_text removes a range", mouse.Text.StartsWith(" ") || mouse.Text.Length < 6);
        mouse.Clear();
        T.Check("clear empties it", mouse.Text == "");
        int toggles = 0;
        mouse.EditingToggled += _ => toggles++;
        mouse.ReleaseFocus();
        mouse.GrabFocus();
        T.Check("gaining and losing focus toggles editing", toggles == 2);
    }

    // ---- TextEdit ----

    private static TextEdit MakeEdit(string text)
    {
        var te = new TextEdit { Text = text };
        Root.AddChild(te);
        te.Position = new Vector2(10, 10);
        te.Size = new Vector2(200, 100);
        Settle();
        te.GrabFocus();
        return te;
    }

    private static void TextEditBehaviour()
    {
        TextEdit te = MakeEdit("one\ntwo\nthree");
        int changed = 0, moved = 0, set = 0;
        te.TextChanged += () => changed++;
        te.CaretChanged += () => moved++;
        te.TextSet += () => set++;
        T.Check("lines are split at newlines and the caret starts at (0, 0)", te.GetLineCount() == 3 && te.GetLine(1) == "two" && te.GetCaretLine() == 0 && te.GetCaretColumn() == 0);
        Press(Key.End);
        Press(Key.Down);
        Press(Key.Down);
        T.Check("End goes to the line end; Down keeps the column where the line allows", te.GetCaretLine() == 2 && te.GetCaretColumn() == 3, $"{te.GetCaretLine()},{te.GetCaretColumn()}");
        Type("X");
        T.Check("typing inserts and emits text_changed", te.GetLine(2) == "thrXee" && changed == 1);
        Press(Key.Enter);
        T.Check("Enter splits the line at the caret", te.GetLineCount() == 4 && te.GetLine(2) == "thrX" && te.GetLine(3) == "ee" && te.GetCaretLine() == 3 && te.GetCaretColumn() == 0);
        Press(Key.Backspace);
        T.Check("backspace at the start of a line joins it to the one above", te.GetLineCount() == 3 && te.GetLine(2) == "thrXee" && te.GetCaretColumn() == 4);
        T.Check("caret_changed was emitted as the caret moved", moved >= 4);
        Key_(Key.Home, true, ctrl: true);
        Key_(Key.Down, true, shift: true);
        Key_(Key.Down, true, shift: true);
        T.Check("shift and Down select over lines", te.HasSelection() && te.GetSelectedText() == "one\ntwo\n" && te.GetSelectionToLine() == 2, $"'{te.GetSelectedText()}'");
        Key_(Key.C, true, ctrl: true);
        Key_(Key.End, true, ctrl: true);
        Key_(Key.V, true, ctrl: true);
        T.Check("copy and paste a multi-line selection", te.GetLineCount() == 5 && te.GetLine(2) == "thrXeeone" && te.GetLine(4) == "", te.Text.Replace("\n", "|"));
        Key_(Key.Z, true, ctrl: true);
        T.Check("undo brings the text back", te.Text == "one\ntwo\nthrXee" && te.HasRedo());
        te.Select(0, 1, 1, 2);
        T.Check("select(origin, caret) spans lines", te.GetSelectedText() == "ne\ntw");
        Type("-");
        T.Check("typing over a multi-line selection joins the lines", te.Text == "o-o\nthrXee" || te.Text.StartsWith("o-"), te.Text.Replace("\n", "|"));
        te.Undo();
        te.Text = "alpha beta\nbeta gamma";
        T.Check("setting the text emits text_set", set == 1);
        Vector2I found = te.Search("beta", 0, 0, 0);
        T.Check("search finds the first match as (column, line)", found == new Vector2I(6, 0), $"{found}");
        found = te.Search("beta", 0, 0, 7);
        T.Check("and the next one", found == new Vector2I(0, 1), $"{found}");
        found = te.Search("BETA", (uint)TextEdit.SearchFlags.MatchCase, 0, 0);
        T.Check("match case refuses a different case", found == new Vector2I(-1, -1));
        found = te.Search("beta", (uint)TextEdit.SearchFlags.Backwards, 1, 4);
        T.Check("backwards finds the one before", found == new Vector2I(0, 1) || found == new Vector2I(6, 0), $"{found}");
        te.SetCaretLine(1);
        te.SetCaretColumn(4);
        te.InsertTextAtCaret("\nnew");
        T.Check("insert_text_at_caret takes newlines", te.GetLineCount() == 3 && te.GetLine(1) == "beta" && te.GetLine(2) == "new gamma", te.Text.Replace("\n", "|"));
        te.RemoveText(0, 5, 1, 2);
        T.Check("remove_text joins across lines", te.GetLine(0) == "alphata" && te.GetLineCount() == 2, te.Text.Replace("\n", "|"));
        te.SetLine(0, "zero");
        T.Check("set_line replaces one line", te.GetLine(0) == "zero");
        te.Clear();
        T.Check("clear leaves one empty line", te.GetLineCount() == 1 && te.Text == "");

        // wrapping
        TextEdit wrap = MakeEdit("aaaa bbbb cccc dddd eeee ffff gggg");
        wrap.Position = new Vector2(10, 200);
        Settle();
        T.Check("without wrapping a long line is one visual line", wrap.GetTotalVisibleLineCount() == 1);
        wrap.WrapMode = TextEdit.LineWrappingMode.Boundary;
        T.Check("with it, 34 characters (272 px) in 192 px take two visual lines", wrap.GetTotalVisibleLineCount() == 2, $"{wrap.GetTotalVisibleLineCount()}");
        T.Check("the view holds four lines (100 px of 23.2 less the margins)", wrap.GetVisibleLineCount() == 3 || wrap.GetVisibleLineCount() == 4, $"{wrap.GetVisibleLineCount()}");
        TextEdit ph = new TextEdit { PlaceholderText = "type" };
        Root.AddChild(ph);
        T.Check("an empty editor draws its placeholder", Texts(ph).Count == 1 && Texts(ph)[0].Text == "type");

        // mouse
        TextEdit mouse = MakeEdit("hello\nworld");
        mouse.Position = new Vector2(10, 400);
        Settle();
        Tap(10 + 4 + 17, 400 + 4 + 23.2f + 5);
        T.Check("a click puts the caret on that line at that column", mouse.GetCaretLine() == 1 && mouse.GetCaretColumn() == 2, $"{mouse.GetCaretLine()},{mouse.GetCaretColumn()}");
        Wheel(20, 410, MouseButton.WheelDown);
        T.Check("the wheel scrolls", mouse.ScrollVertical >= 0);
        T.Check("an uneditable editor refuses typing", Type_(mouse));
    }

    private static bool Type_(TextEdit te)
    {
        te.Editable = false;
        te.GrabFocus();
        string before = te.Text;
        Type("q");
        return te.Text == before;
    }

    // ---- ItemList ----

    private static ItemList MakeList(params string[] items)
    {
        var il = new ItemList();
        foreach (string s in items) il.AddItem(s);
        Root.AddChild(il);
        il.Position = new Vector2(10, 10);
        il.Size = new Vector2(100, 100);
        Settle();
        return il;
    }

    private static float RowY(int row) => 10 + 4 + row * 23.2f + 6f;

    private static void ItemListBehaviour()
    {
        ItemList il = MakeList("a", "b", "c");
        var log = new List<string>();
        il.ItemSelected += i => log.Add("sel:" + i);
        il.ItemClicked += (i, p, b) => log.Add($"click:{i}:{b}");
        il.MultiSelected += (i, on) => log.Add($"multi:{i}:{on}");
        il.ItemActivated += i => log.Add("act:" + i);
        il.EmptyClicked += (p, b) => log.Add("empty:" + b);
        T.Check("items are counted and read back", il.ItemCount == 3 && il.GetItemText(1) == "b" && il.GetItemAtPosition(new Vector2(20, RowY(1) - 10)) == 1);
        Tap(20, RowY(1));
        T.Check("a click clicks then selects the item (single mode)", log.SequenceEqual(new[] { "click:1:1", "sel:1" }) && il.IsSelected(1) && il.GetSelectedItems().SequenceEqual(new[] { 1 }), string.Join(",", log));
        log.Clear();
        Tap(20, RowY(1));
        T.Check("clicking the selected item again selects nothing new (allow_reselect off)", log.SequenceEqual(new[] { "click:1:1" }), string.Join(",", log));
        log.Clear();
        Tap(20, RowY(2));
        T.Check("selecting another deselects the first", il.IsSelected(2) && !il.IsSelected(1) && log.Contains("sel:2"));
        log.Clear();
        Click(20, RowY(0), true, MouseButton.Left, dbl: true);
        Click(20, RowY(0), false);
        T.Check("a double click also activates", log.Contains("act:0"), string.Join(",", log));
        log.Clear();
        Tap(20, 95);
        T.Check("a click below the items is an empty click", log.SequenceEqual(new[] { "empty:1" }), string.Join(",", log));
        log.Clear();
        il.GrabFocus();
        Press(Key.Down);
        T.Check("Down selects the next item", il.IsSelected(1) && log.Contains("sel:1"));
        Press(Key.Enter);
        T.Check("accept activates it", log.Contains("act:1"));
        Press(Key.End);
        T.Check("End goes to the last", il.IsSelected(2));
        Press(Key.Home);
        T.Check("Home to the first", il.IsSelected(0));
        Type("c");
        T.Check("a letter jumps to the item that starts with it", il.IsSelected(2), $"{string.Join(",", il.GetSelectedItems())}");

        il.SelectMode = ItemList.SelectModeEnum.Multi;
        il.DeselectAll();
        log.Clear();
        Tap(20, RowY(0));
        Tap(20, RowY(2), ctrl: true);
        T.Check("multi mode: a click selects one, ctrl adds another (multi_selected)", il.GetSelectedItems().SequenceEqual(new[] { 0, 2 }) && log.Count(l => l.StartsWith("multi:")) == 2, string.Join(",", log));
        Tap(20, RowY(0), ctrl: true);
        T.Check("ctrl on a selected item removes it", il.GetSelectedItems().SequenceEqual(new[] { 2 }));
        Tap(20, RowY(0));
        Tap(20, RowY(2), shift: true);
        T.Check("shift selects the range", il.GetSelectedItems().SequenceEqual(new[] { 0, 1, 2 }), string.Join(",", il.GetSelectedItems()));
        il.SelectMode = ItemList.SelectModeEnum.Toggle;
        il.DeselectAll();
        Tap(20, RowY(1));
        Tap(20, RowY(2));
        Tap(20, RowY(1));
        T.Check("toggle mode: each click flips its item", il.GetSelectedItems().SequenceEqual(new[] { 2 }), string.Join(",", il.GetSelectedItems()));

        il.SelectMode = ItemList.SelectModeEnum.Single;
        il.DeselectAll();
        il.SetItemDisabled(1, true);
        log.Clear();
        Tap(20, RowY(1));
        T.Check("a disabled item cannot be selected", !il.IsSelected(1) && !log.Any(l => l.StartsWith("sel")));
        il.SetItemDisabled(1, false);
        il.SetItemSelectable(1, false);
        Tap(20, RowY(1));
        T.Check("neither can an unselectable one, though it is clicked", !il.IsSelected(1) && log.Contains("click:1:1"));
        il.SetItemTooltip(0, "tip");
        T.Check("an item's tooltip is the control's at that point", il.GetTooltip(new Vector2(20, RowY(0))) == "tip");
        il.RemoveItem(0);
        T.Check("remove_item drops one", il.ItemCount == 2);
        il.Clear();
        T.Check("clear drops all", il.ItemCount == 0);

        ItemList columns = MakeList("a", "b", "c", "d", "e");
        columns.MaxColumns = 2;
        columns.Position = new Vector2(300, 10);
        Settle();
        T.Check("with two columns the items fill rows of two: 'd' is second in the second row", IsRect(columns.GetItemRect(3), 4 + 46, 4 + 23.2f, 46, 23.2f, 0.5f), $"{columns.GetItemRect(3)}");
        ItemList auto = MakeList("one", "twenty");
        auto.AutoHeight = true;
        auto.AutoWidth = true;
        Settle();
        T.Check("auto width and height size the control to the items: 6 characters, two rows, 8 margins", Is(auto.GetCombinedMinimumSize(), 48 + 8, 2 * 23.2f + 8, 0.2f), $"{auto.GetCombinedMinimumSize()}");
        T.Check("an item's text is trimmed with an ellipsis when it does not fit", Texts(MakeList("a very long item text indeed")).Any(t => t.Text.EndsWith('…')));
    }

    // ---- Tree ----

    private static Tree MakeTree(out TreeItem root, out TreeItem a, out TreeItem grand, out TreeItem b)
    {
        var tree = new Tree { Columns = 2 };
        Root.AddChild(tree);
        tree.Position = new Vector2(10, 10);
        tree.Size = new Vector2(200, 200);
        root = tree.CreateItem()!;
        root.SetText(0, "Root");
        a = tree.CreateItem(root)!;
        a.SetText(0, "A");
        a.SetText(1, "a1");
        grand = tree.CreateItem(a)!;
        grand.SetText(0, "G");
        b = tree.CreateItem(root)!;
        b.SetText(0, "B");
        Settle();
        return tree;
    }

    private static void TreeBehaviour()
    {
        Tree tree = MakeTree(out TreeItem root, out TreeItem a, out TreeItem grand, out TreeItem b);
        var log = new List<string>();
        tree.ItemSelected += () => log.Add("item");
        tree.CellSelected += () => log.Add("cell");
        tree.ItemCollapsed += i => log.Add("collapsed:" + i.GetText(0));
        tree.ItemActivated += () => log.Add("act");
        tree.ItemEdited += () => log.Add("edited");
        tree.EmptyClicked += (p, mb) => log.Add("empty");
        tree.MultiSelected += (i, c, on) => log.Add($"multi:{i.GetText(0)}:{c}:{on}");
        T.Check("the structure reads back: parent, children, order, count", tree.GetRoot() == root && a.GetParent() == root && root.GetChildCount() == 2 && a.GetNext() == b && b.GetPrev() == a && b.GetIndex() == 1 && a.GetFirstChild() == grand);
        T.Check("walking the tree: in order, and only the visible rows", root.GetNextInTree() == a && a.GetNextInTree() == grand && grand.GetNextVisible() == b && b.GetPrevVisible() == grand);
        float rowH = 23.2f;
        float y0 = 10 + 4;
        T.Check("the item under a point is the row it is in", tree.GetItemAtPosition(new Vector2(30, y0 + rowH * 1 + 5)) == a && tree.GetItemAtPosition(new Vector2(30, y0 + rowH * 3 + 5)) == b && tree.GetItemAtPosition(new Vector2(30, 190)) == null);
        T.Check("and the column is the share of the width: 96 each", tree.GetColumnAtPosition(new Vector2(10 + 4 + 10, 30)) == 0 && tree.GetColumnAtPosition(new Vector2(10 + 4 + 100, 30)) == 1 && tree.GetColumnWidth(0) == 96);

        Tap(10 + 4 + 50, y0 + rowH + 5);
        T.Check("a click selects the cell: cell_selected then item_selected", log.SequenceEqual(new[] { "cell", "item" }) && tree.GetSelected() == a && tree.GetSelectedColumn() == 0 && a.IsSelected(0), string.Join(",", log));
        log.Clear();
        Tap(10 + 4 + 120, y0 + rowH + 5);
        T.Check("the other column of the same item selects again (a new cell)", log.SequenceEqual(new[] { "cell", "item" }) && tree.GetSelectedColumn() == 1 && !a.IsSelected(0), string.Join(",", log));
        log.Clear();
        Tap(10 + 4 + 120, y0 + rowH + 5);
        T.Check("the same cell again selects nothing new", log.Count == 0);
        // folding: the arrow of A at x = indent(depth 1) = 16 + 16 = 32, 16 wide to its left
        Tap(10 + 4 + 24, y0 + rowH + 5);
        T.Check("a click on the fold arrow collapses and says item_collapsed", a.Collapsed && log.SequenceEqual(new[] { "collapsed:A" }), string.Join(",", log));
        T.Check("a collapsed item hides its children from the walk", a.GetNextVisible() == b && tree.GetItemAtPosition(new Vector2(30, y0 + rowH * 2 + 5)) == b);
        Tap(10 + 4 + 24, y0 + rowH + 5);
        T.Check("and expands again", !a.Collapsed && a.GetNextVisible() == grand);
        log.Clear();
        tree.GrabFocus();
        tree.SetSelected(root, 0);
        log.Clear();
        Press(Key.Down);
        T.Check("Down moves the selection along the visible rows", tree.GetSelected() == a);
        Press(Key.Down);
        Press(Key.Down);
        T.Check("past the grandchild to B", tree.GetSelected() == b);
        Press(Key.Up);
        Press(Key.Up);
        Press(Key.Left);
        T.Check("Left on an expanded item collapses it", tree.GetSelected() == a && a.Collapsed);
        Press(Key.Right);
        T.Check("Right expands it", !a.Collapsed);
        Press(Key.Down);
        Press(Key.Left);
        T.Check("Left on a leaf goes to its parent", tree.GetSelected() == a);
        log.Clear();
        Press(Key.Enter);
        T.Check("accept activates", log.SequenceEqual(new[] { "act" }));
        log.Clear();
        Click(10 + 4 + 10, y0 + rowH + 5, true, MouseButton.Left, dbl: true);
        Click(10 + 4 + 10, y0 + rowH + 5, false);
        T.Check("a double click activates too", log.Contains("act"));
        log.Clear();
        Tap(10 + 4 + 100, 190);
        T.Check("below the rows is an empty click", log.SequenceEqual(new[] { "empty" }));

        // cells
        a.SetCellMode(1, TreeItem.TreeCellMode.Check);
        a.SetEditable(1, true);
        log.Clear();
        float cx = 10 + 4 + 96 + 3;
        Tap(cx, y0 + rowH + 5);
        T.Check("a check cell toggles when its box is clicked, and says item_edited", a.IsChecked(1) && log.SequenceEqual(new[] { "edited" }), string.Join(",", log));
        Tap(cx, y0 + rowH + 5);
        T.Check("and back", !a.IsChecked(1));
        a.SetCellMode(0, TreeItem.TreeCellMode.Range);
        a.SetRangeConfig(0, 0, 10, 2);
        a.SetRange(0, 5.1);
        T.Check("a range cell snaps to its step and clamps", a.GetRange(0) == 6);
        a.SetCellMode(0, TreeItem.TreeCellMode.String);
        a.SetMetadata(0, "meta");
        a.SetCustomColor(0, new Color(1, 0, 0, 1));
        a.SetSuffix(0, "!");
        T.Check("cell data: metadata, colour, suffix", a.GetMetadata(0).AsString() == "meta" && a.GetCustomColor(0) == new Color(1, 0, 0, 1) && a.GetSuffix(0) == "!");
        a.SetSelectable(1, false);
        log.Clear();
        a.Deselect(0);
        tree.DeselectAll();
        Tap(cx, y0 + rowH + 5);
        T.Check("an unselectable cell cannot be selected", tree.GetSelected() == null);
        // structure edits
        TreeItem c = tree.CreateItem(root, 0)!;
        c.SetText(0, "C");
        T.Check("create_item at an index inserts there", root.GetChild(0) == c && c.GetIndex() == 0);
        c.MoveAfter(b);
        T.Check("move_after reorders", root.GetChild(root.GetChildCount() - 1) == c);
        c.MoveBefore(a);
        T.Check("move_before too", root.GetChild(0) == c && a.GetPrev() == c);
        c.Free();
        T.Check("free takes the item out", root.GetChildCount() == 2 && root.GetChild(0) == a);
        a.AddChild(b);
        T.Check("add_child re-parents", b.GetParent() == a && root.GetChildCount() == 1);
        tree.Clear();
        T.Check("clear drops everything", tree.GetRoot() == null);

        // titles, hidden root, columns
        var t2 = new Tree { Columns = 2, ColumnTitlesVisible = true, HideRoot = true };
        Root.AddChild(t2);
        t2.Position = new Vector2(300, 10);
        t2.Size = new Vector2(200, 100);
        TreeItem r2 = t2.CreateItem()!;
        TreeItem first = t2.CreateItem(r2)!;
        first.SetText(0, "First");
        t2.SetColumnTitle(0, "Name");
        t2.SetColumnTitle(1, "Value");
        t2.SetColumnExpand(0, false);
        t2.SetColumnCustomMinimumWidth(0, 60);
        Settle();
        T.Check("titles are kept and a fixed column keeps its width, the other takes the rest", t2.GetColumnTitle(1) == "Value" && t2.GetColumnWidth(0) == 60 && t2.GetColumnWidth(1) == 132, $"{t2.GetColumnWidth(0)} {t2.GetColumnWidth(1)}");
        T.Check("a hidden root leaves its children at the top; the title row pushes them down 23.2", IsRect(t2.GetItemAreaRect(first), 4, 4 + 23.2f, 192, 23.2f, 0.2f), $"{t2.GetItemAreaRect(first)}");
        var titleLog = new List<string>();
        t2.ColumnTitleClicked += (col, btn) => titleLog.Add($"{col}:{btn}");
        Tap(300 + 4 + 80, 10 + 4 + 5);
        T.Check("a click on a title says column_title_clicked", titleLog.SequenceEqual(new[] { "1:1" }), string.Join(",", titleLog));
        t2.SelectMode = Tree.SelectModeEnum.Row;
        Tap(300 + 4 + 80, 10 + 4 + 23.2f + 5);
        T.Check("row select mode selects every cell of the item", first.IsSelected(0) && first.IsSelected(1));
        t2.SelectMode = Tree.SelectModeEnum.Multi;
        TreeItem second = t2.CreateItem(r2)!;
        second.SetText(0, "Second");
        t2.DeselectAll();
        var mlog = new List<string>();
        t2.MultiSelected += (i, col, on) => mlog.Add($"{i.GetText(0)}:{col}:{on}");
        Tap(300 + 4 + 20, 10 + 4 + 23.2f + 5);
        Tap(300 + 4 + 20, 10 + 4 + 2 * 23.2f + 5, ctrl: true);
        T.Check("multi mode: ctrl adds cells and each says multi_selected", mlog.SequenceEqual(new[] { "First:0:True", "Second:0:True" }) && t2.GetNextSelected(null) == first && t2.GetNextSelected(first) == second, string.Join(",", mlog));
        TreeItem button = t2.CreateItem(r2)!;
        var btnLog = new List<string>();
        t2.ButtonClicked += (i, col, id, mb) => btnLog.Add($"{i.GetText(0)}:{col}:{id}");
        button.SetText(0, "Btn");
        button.AddButton(1, new Sizeable(16, 16, 0), 42);
        Settle();
        Tap(300 + 4 + 60 + 132 - 8, 10 + 4 + 3 * 23.2f + 5);
        T.Check("a cell button says button_clicked with its id", btnLog.SequenceEqual(new[] { "Btn:1:42" }), string.Join(",", btnLog));
        T.Check("text cells are not edited in place", !t2.EditSelected());
    }

    // ---- tabs ----

    private static void TabBehaviour()
    {
        var bar = new TabBar();
        bar.AddTab("One");
        bar.AddTab("Two");
        bar.AddTab("Three");
        Root.AddChild(bar);
        bar.Position = new Vector2(10, 10);
        bar.Size = new Vector2(300, 28);
        Settle();
        var log = new List<string>();
        bar.TabClicked += t => log.Add("clicked:" + t);
        bar.TabSelected += t => log.Add("selected:" + t);
        bar.TabChanged += t => log.Add("changed:" + t);
        T.Check("the tabs' rectangles: each is its text and 12 px of margin wide, a line and 8 high", IsRect(bar.GetTabRect(0), 0, 0, 36, 27.2f) && IsRect(bar.GetTabRect(1), 36, 0, 36, 27.2f) && IsRect(bar.GetTabRect(2), 72, 0, 52, 27.2f), $"{bar.GetTabRect(1)}");
        T.Check("the bar is as tall as a tab", T.Near(bar.GetCombinedMinimumSize().Y, 27.2f) && bar.TabCount == 3 && bar.CurrentTab == 0);
        Tap(10 + 50, 20);
        T.Check("a click: tab_clicked, tab_selected, then tab_changed", log.SequenceEqual(new[] { "clicked:1", "selected:1", "changed:1" }) && bar.CurrentTab == 1, string.Join(",", log));
        log.Clear();
        Tap(10 + 50, 20);
        T.Check("clicking the current tab again does not change", log.SequenceEqual(new[] { "clicked:1", "selected:1" }), string.Join(",", log));
        bar.SetTabDisabled(2, true);
        log.Clear();
        Tap(10 + 90, 20);
        T.Check("a disabled tab is clicked but not selected", log.SequenceEqual(new[] { "clicked:2" }) && bar.CurrentTab == 1 && bar.IsTabDisabled(2), string.Join(",", log));
        T.Check("select_next_available skips it and wraps", bar.SelectNextAvailable() && bar.CurrentTab == 0);
        bar.SetTabHidden(1, true);
        T.Check("a hidden tab has no room and cannot be picked", bar.GetTabRect(1).Size.X == 0 && bar.GetTabIdxAtPoint(new Vector2(40, 10)) != 1);
        bar.SetTabHidden(1, false);
        bar.TabAlignment = TabBar.AlignmentMode.Center;
        T.Check("centred tabs sit in the middle: (300 - 124) / 2 = 88", T.Near(bar.GetTabRect(0).Position.X, 88f), $"{bar.GetTabRect(0)}");
        bar.TabAlignment = TabBar.AlignmentMode.Right;
        T.Check("right aligned: 300 - 124", T.Near(bar.GetTabRect(0).Position.X, 176f));
        bar.TabAlignment = TabBar.AlignmentMode.Left;
        bar.TabCloseDisplayPolicy = TabBar.CloseButtonDisplayPolicy.ShowAlways;
        log.Clear();
        var closed = new List<long>();
        bar.TabClosePressed += t => closed.Add(t);
        Rect2 r1 = bar.GetTabRect(1);
        Tap(10 + r1.End.X - 4, 20);
        T.Check("the close button says tab_close_pressed and does not select", closed.SequenceEqual(new long[] { 1 }) && !log.Contains("selected:1"), $"{string.Join(",", closed)} {string.Join(",", log)}");
        bar.RemoveTab(0);
        T.Check("remove_tab drops it and the selection follows its tab", bar.TabCount == 2 && bar.GetTabTitle(0) == "Two");
        bar.SetTabTitle(0, "Zwei");
        T.Check("titles are editable", bar.GetTabTitle(0) == "Zwei");
        bar.SetTabTooltip(0, "tip");
        T.Check("and tooltips", bar.GetTooltip(new Vector2(5, 5)) == "tip");

        // container
        var tc = new TabContainer();
        var pageA = new Control { Name = "Alpha" };
        var pageB = new Control { Name = "Beta" };
        var pageC = new Control { Name = "Gamma" };
        pageA.CustomMinimumSize = new Vector2(50, 20);
        tc.AddChild(pageA);
        tc.AddChild(pageB);
        tc.AddChild(pageC);
        Root.AddChild(tc);
        tc.Position = new Vector2(10, 100);
        tc.Size = new Vector2(200, 100);
        Settle();
        var tlog = new List<string>();
        tc.TabChanged += t => tlog.Add("changed:" + t);
        tc.TabSelected += t => tlog.Add("selected:" + t);
        T.Check("the first page shows; the others are hidden; titles are the children's names", tc.CurrentTab == 0 && pageA.Visible && !pageB.Visible && !pageC.Visible && tc.GetTabTitle(1) == "Beta" && tc.GetTabCount() == 3);
        T.Check("the page fills the area under the tabs, inside the panel's 4 px margins", IsRect(pageA.GetRect(), 4, 27.2f + 4, 192, 100 - 27.2f - 8, 0.1f), $"{pageA.GetRect()}");
        Tap(10 + 70, 100 + 10);
        T.Check("clicking a tab (Beta, after the 52 px of Alpha) shows that page and says tab_selected, tab_changed", tc.CurrentTab == 1 && !pageA.Visible && pageB.Visible && tlog.SequenceEqual(new[] { "selected:1", "changed:1" }), string.Join(",", tlog));
        T.Check("get_current_tab_control and get_tab_idx_from_control", tc.GetCurrentTabControl() == pageB && tc.GetTabIdxFromControl(pageC) == 2);
        tc.SetTabTitle(2, "Third");
        T.Check("set_tab_title overrides the name", tc.GetTabTitle(2) == "Third" && pageC.Name == "Gamma");
        tc.SetTabDisabled(2, true);
        tlog.Clear();
        tc.CurrentTab = 0;
        T.Check("setting current_tab from code emits too", tlog.SequenceEqual(new[] { "selected:0", "changed:0" }));
        T.Check("a disabled tab is skipped by next-available", tc.SelectNextAvailable() && tc.CurrentTab == 1 && tc.SelectNextAvailable() && tc.CurrentTab == 0);
        tc.TabsVisible = false;
        Settle();
        T.Check("without the tab row the page starts at the panel's margin", T.Near(pageA.Position.Y, 4f), $"{pageA.Position}");
        tc.TabsVisible = true;
        tc.TabsPosition = TabContainer.TabPositionEnum.Bottom;
        Settle();
        T.Check("tabs at the bottom put the page at the top", T.Near(pageA.Position.Y, 4f) && Draw(tc).Count > 3);
        T.Check("the minimum size is the shown page plus the tab row and the panel", T.Near(tc.GetCombinedMinimumSize().Y, 20 + 8 + 27.2f, 0.1f), $"{tc.GetCombinedMinimumSize()}");
        var newPage = new Control { Name = "Delta" };
        tc.AddChild(newPage);
        Settle();
        T.Check("a page added later joins the tabs", tc.GetTabCount() == 4 && tc.GetTabTitle(3) == "Delta" && !newPage.Visible);
        pageA.SetMeta("_tab_name", "Renamed");
        tc.SetTabHidden(1, true);
        T.Check("the scene's metadata names a tab, and tabs can be hidden", tc.GetTabTitle(0) == "Renamed" && tc.IsTabHidden(1));
    }

    // ---- layout ----

    private static Control Expander(string name, float minW, float minH, Control.SizeFlags h = Control.SizeFlags.ExpandFill, Control.SizeFlags v = Control.SizeFlags.ExpandFill)
    {
        return new Control { Name = name, CustomMinimumSize = new Vector2(minW, minH), SizeFlagsHorizontal = h, SizeFlagsVertical = v };
    }

    private static void LayoutBehaviour()
    {
        var split = new HSplitContainer();
        Control a = Expander("A", 50, 10), b = Expander("B", 50, 10);
        split.AddChild(a);
        split.AddChild(b);
        Root.AddChild(split);
        split.Position = new Vector2(10, 10);
        split.Size = new Vector2(300, 100);
        Settle();
        T.Check("two expanding children share the room less the 12 px separator", IsRect(a.GetRect(), 0, 0, 144, 100) && IsRect(b.GetRect(), 156, 0, 144, 100), $"{a.GetRect()} {b.GetRect()}");
        split.SplitOffset = 20;
        Settle();
        T.Check("split_offset moves the divider", IsRect(a.GetRect(), 0, 0, 164, 100) && T.Near(b.Position.X, 176f), $"{a.GetRect()} {b.GetRect()}");
        split.SplitOffset = -500;
        Settle();
        T.Check("and cannot squeeze a child below its minimum", T.Near(a.Size.X, 50f));
        split.SplitOffset = 0;
        Settle();
        var dragged = new List<long>();
        split.Dragged += o => dragged.Add(o);
        Click(10 + 150, 50, true);
        Move(10 + 170, 50, 20, 0);
        Click(10 + 170, 50, false);
        T.Check("dragging the divider moves it and says dragged with the offset", split.SplitOffset == 20 && dragged.Count == 1 && dragged[0] == 20, $"{split.SplitOffset} {string.Join(",", dragged)}");
        T.Check("the minimum size adds the separator", Is(split.GetCombinedMinimumSize(), 112, 10));
        b.SizeFlagsHorizontal = Control.SizeFlags.Fill;
        split.SplitOffset = 0;
        Settle();
        T.Check("with only the first expanding the second keeps its minimum", T.Near(a.Size.X, 238f) && T.Near(b.Size.X, 50f), $"{a.Size} {b.Size}");
        split.Collapsed = true;
        T.Check("a collapsed split cannot be dragged", !Dragging(split));
        var vsplit = new VSplitContainer();
        Control va = Expander("VA", 10, 20), vb = Expander("VB", 10, 20);
        vsplit.AddChild(va);
        vsplit.AddChild(vb);
        Root.AddChild(vsplit);
        vsplit.Size = new Vector2(100, 112);
        Settle();
        T.Check("a vertical split divides the height", IsRect(va.GetRect(), 0, 0, 100, 50) && IsRect(vb.GetRect(), 0, 62, 100, 50), $"{va.GetRect()} {vb.GetRect()}");

        var flow = new HFlowContainer();
        var kids = new[] { Expander("F1", 40, 20, Control.SizeFlags.Fill, Control.SizeFlags.Fill), Expander("F2", 40, 20, Control.SizeFlags.Fill, Control.SizeFlags.Fill), Expander("F3", 40, 20, Control.SizeFlags.Fill, Control.SizeFlags.Fill) };
        foreach (Control k in kids) flow.AddChild(k);
        Root.AddChild(flow);
        flow.Size = new Vector2(100, 10);
        Settle();
        Settle();
        T.Check("children flow along the row and wrap when the next does not fit: 40 + 4 + 40 = 84, a third would be 128", Is(kids[0].Position, 0, 0) && Is(kids[1].Position, 44, 0) && Is(kids[2].Position, 0, 24), $"{kids[0].Position} {kids[1].Position} {kids[2].Position}");
        T.Check("the container is as tall as its rows: 20 + 4 + 20", T.Near(flow.GetCombinedMinimumSize().Y, 44f), $"{flow.GetCombinedMinimumSize()}");
        flow.Alignment = FlowContainer.AlignmentMode.End;
        Settle();
        T.Check("end alignment pushes a row to the right: 16 px free", T.Near(kids[0].Position.X, 16f) && T.Near(kids[2].Position.X, 60f), $"{kids[0].Position} {kids[2].Position}");
        flow.Alignment = FlowContainer.AlignmentMode.Begin;
        var vflow = new VFlowContainer();
        var vk = new[] { Expander("V1", 20, 30, Control.SizeFlags.Fill, Control.SizeFlags.Fill), Expander("V2", 20, 30, Control.SizeFlags.Fill, Control.SizeFlags.Fill) };
        foreach (Control k in vk) vflow.AddChild(k);
        Root.AddChild(vflow);
        vflow.Size = new Vector2(10, 50);
        Settle();
        Settle();
        T.Check("a vertical flow wraps into columns: 30 + 4 + 30 > 50", Is(vk[0].Position, 0, 0) && Is(vk[1].Position, 24, 0), $"{vk[0].Position} {vk[1].Position}");

        var ratio = new AspectRatioContainer { Ratio = 2f };
        Control child = Expander("R", 1, 1);
        ratio.AddChild(child);
        Root.AddChild(ratio);
        ratio.Size = new Vector2(100, 100);
        Settle();
        T.Check("fit keeps the ratio inside the container, centred: 100 x 50 at y = 25", IsRect(child.GetRect(), 0, 25, 100, 50), $"{child.GetRect()}");
        ratio.StretchMode = AspectRatioContainer.StretchModeEnum.Cover;
        Settle();
        T.Check("cover fills it: 200 x 100, centred", IsRect(child.GetRect(), -50, 0, 200, 100), $"{child.GetRect()}");
        ratio.StretchMode = AspectRatioContainer.StretchModeEnum.HeightControlsWidth;
        ratio.AlignmentHorizontal = AspectRatioContainer.AlignmentMode.Begin;
        Settle();
        T.Check("height controls width: 200 x 100 from the left", IsRect(child.GetRect(), 0, 0, 200, 100), $"{child.GetRect()}");
        ratio.StretchMode = AspectRatioContainer.StretchModeEnum.WidthControlsHeight;
        Settle();
        T.Check("width controls height: 100 x 50, centred vertically", IsRect(child.GetRect(), 0, 25, 100, 50), $"{child.GetRect()}");

        var hs = new HSeparator();
        var vs = new VSeparator();
        Root.AddChild(hs);
        Root.AddChild(vs);
        Settle();
        T.Check("separators are the theme's separation thick, and pass the pointer", hs.GetCombinedMinimumSize().Y >= 4f && vs.GetCombinedMinimumSize().X >= 4f && hs.MouseFilter == Control.MouseFilterEnum.Pass);
        T.Check("they draw their line", Draw(hs).Count(c => c.Type == CanvasItem.DrawCommand.Kind.StyleBox) == 1);
    }

    private static bool Dragging(SplitContainer s)
    {
        Click(s.GlobalPosition.X + s.FirstSize() + 2, s.GlobalPosition.Y + 5, true);
        int before = s.SplitOffset;
        Move(s.GlobalPosition.X + s.FirstSize() + 22, s.GlobalPosition.Y + 5, 20, 0);
        Click(s.GlobalPosition.X + s.FirstSize() + 22, s.GlobalPosition.Y + 5, false);
        return s.SplitOffset != before;
    }

    // ---- colour picker ----

    private static void ColorPickerBehaviour()
    {
        var cp = new ColorPicker();
        Root.AddChild(cp);
        cp.Position = new Vector2(10, 10);
        Settle();
        cp.Size = cp.GetCombinedMinimumSize();
        var log = new List<Color>();
        cp.ColorChanged += c => log.Add(c);
        cp.Color = new Color(1, 0, 0, 1);
        T.Check("set_pick_color emits nothing; the colour is held as hue, saturation, value and alpha", log.Count == 0 && T.Near(cp.Hue, 0f) && T.Near(cp.Saturation, 1f) && T.Near(cp.Value, 1f) && cp.Color == new Color(1, 0, 0, 1));
        // the square is (4, 4, 256, 256) in the control
        Tap(10 + 4 + 128, 10 + 4 + 128);
        T.Check("a click in the square sets saturation and value from the position and says color_changed", log.Count >= 1 && T.Near(cp.Saturation, 0.5f, 0.01f) && T.Near(cp.Value, 0.5f, 0.01f), $"{cp.Saturation} {cp.Value} {log.Count}");
        Click(10 + 4 + 255, 10 + 4 + 1, true);
        Move(10 + 4 + 300, 10 + 4 - 20, 44, -20);
        Click(10 + 4 + 300, 10 + 4 - 20, false);
        T.Check("dragging outside the square clamps to the corner: full saturation and value", T.Near(cp.Saturation, 1f, 0.01f) && T.Near(cp.Value, 1f, 0.01f));
        Tap(10 + 4 * 2 + 256 + 15, 10 + 4 + 128);
        T.Check("the hue bar sets the hue: halfway is 0.5 (cyan)", T.Near(cp.Hue, 0.5f, 0.01f) && T.Near(cp.Color.R, 0f, 0.02f) && T.Near(cp.Color.G, 1f, 0.02f), $"{cp.Hue} {cp.Color}");
        cp.Color = new Color(0.5f, 0.5f, 0.5f, 1);
        T.Check("a grey keeps the hue it had", T.Near(cp.Hue, 0.5f, 0.01f) && T.Near(cp.Saturation, 0f));
        Tap(10 + 4 + 72.5f, 10 + 4 * 2 + 256 + 10);
        T.Check("the alpha bar (290 px wide) sets the alpha from the position: 72.5 is a quarter", T.Near(cp.Alpha, 0.25f, 0.02f) && T.Near(cp.Color.A, 0.25f, 0.02f), $"{cp.Alpha}");
        cp.EditAlpha = false;
        T.Check("without edit_alpha there is no bar to hit", cp.GetCombinedMinimumSize().Y > 0);
        cp.EditAlpha = true;
        var presets = new List<string>();
        cp.PresetAdded += c => presets.Add("add");
        cp.PresetRemoved += c => presets.Add("remove");
        cp.AddPreset(new Color(0, 0, 1, 1));
        cp.AddPreset(new Color(0, 0, 1, 1));
        T.Check("a preset is added once, with preset_added", cp.GetPresets().Length == 1 && presets.SequenceEqual(new[] { "add" }));
        Settle();
        cp.Size = cp.GetCombinedMinimumSize();
        log.Clear();
        float presetY = 10 + 4 * 3 + 256 + 20 + 10;
        Tap(10 + 4 + 10, presetY);
        T.Check("clicking a preset picks it", log.Count == 1 && log[0].B > 0.99f && log[0].R < 0.01f, $"{log.Count}");
        cp.ErasePreset(new Color(0, 0, 1, 1));
        T.Check("erase_preset says preset_removed", cp.GetPresets().Length == 0 && presets.Last() == "remove");
        T.Check("it draws the square, hue bar, alpha bar", Draw(cp).Count(c => c.Type == CanvasItem.DrawCommand.Kind.Rect) > 256);

        var btn = new ColorPickerButton { Color = new Color(0.2f, 0.4f, 0.6f, 1) };
        Root.AddChild(btn);
        btn.Position = new Vector2(400, 10);
        btn.Size = new Vector2(60, 30);
        Settle();
        var blog = new List<Color>();
        btn.ColorChanged += c => blog.Add(c);
        T.Check("the button holds the colour and the picker has it too", btn.Color == new Color(0.2f, 0.4f, 0.6f, 1) && btn.GetPicker().Color == btn.Color);
        List<CanvasItem.DrawCommand> cmds = Draw(btn);
        T.Check("it draws a swatch of the colour", cmds.Any(c => c.Type == CanvasItem.DrawCommand.Kind.Rect && c.Color == btn.Color));
        Tap(410, 20);
        T.Check("a press opens the picker in a popup under it", btn.GetPopup().IsPopupShown && btn.GetPicker().GetParent() == btn.GetPopup());
        btn.GetPicker().SetHsv(0.1f, 1f, 1f);
        T.Check("changing the picker changes the button's colour and says color_changed", blog.Count == 1 && btn.Color == blog[0]);
        Tap(900, 700);
        T.Check("a click outside closes the popup", !btn.GetPopup().IsPopupShown);
    }

    // ---- ScrollContainer ----

    private static void ScrollBars()
    {
        var sc = new ScrollContainer();
        var content = new Control { CustomMinimumSize = new Vector2(100, 400) };
        sc.AddChild(content);
        Root.AddChild(sc);
        sc.Position = new Vector2(10, 10);
        sc.Size = new Vector2(120, 100);
        Settle();
        Settle();
        T.Check("taller content shows the vertical bar", sc.VerticalBarVisible && !sc.HorizontalBarVisible);
        VScrollBar bar = sc.GetVScrollBar();
        T.Check("the bar mirrors the content: max 400, page the view's 100", T.Near((float)bar.MaxValue, 400f) && T.Near((float)bar.Page, 100f), $"{bar.MaxValue} {bar.Page}");
        sc.ScrollVertical = 50;
        T.Check("the bar follows the container", T.Near((float)bar.Value, 50f));
        bar.Value = 120;
        T.Check("and the container follows the bar", sc.ScrollVertical == 120);
        List<CanvasItem.DrawCommand> cmds = Draw(sc);
        T.Check("the container draws its panel, the bar's track and its grabber", cmds.Count(c => c.Type == CanvasItem.DrawCommand.Kind.StyleBox) == 3, $"{cmds.Count}");
        // the track is the 12 px column at the right of the view (108..120), the grabber 25 px of 100 (the page's share) at 120/300 of the travel
        sc.ScrollVertical = 0;
        Click(10 + 114, 10 + 10, true);
        Move(10 + 114, 10 + 40, 0, 30);
        Click(10 + 114, 10 + 40, false);
        T.Check("dragging the grabber scrolls in proportion", sc.ScrollVertical > 60, $"{sc.ScrollVertical}");
        sc.ScrollVertical = 0;
        Tap(10 + 114, 10 + 90);
        T.Check("a press on the track below the grabber pages down by the view", sc.ScrollVertical == 100, $"{sc.ScrollVertical}");
    }

    // ---- the classes ----

    private static void RealClasses()
    {
        string[] names =
        {
            "Label", "RichTextLabel", "BaseButton", "Button", "CheckBox", "CheckButton", "LinkButton", "TextureButton", "OptionButton", "MenuButton", "ColorPickerButton", "Range", "Slider", "HSlider", "VSlider",
            "ScrollBar", "HScrollBar", "VScrollBar", "ProgressBar", "TextureProgressBar", "SpinBox", "LineEdit", "TextEdit", "ItemList", "Tree", "TabBar", "TabContainer", "SplitContainer", "HSplitContainer",
            "VSplitContainer", "FlowContainer", "HFlowContainer", "VFlowContainer", "AspectRatioContainer", "NinePatchRect", "TextureRect", "ColorRect", "Separator", "HSeparator", "VSeparator", "ColorPicker",
            "Popup", "PopupPanel", "PopupMenu", "AcceptDialog", "ConfirmationDialog", "Font", "FontFile", "FontVariation", "SystemFont", "LabelSettings", "ButtonGroup", "Shortcut", "TreeItem",
        };
        var missing = names.Where(n => !ClassDB.ClassExists(n)).ToList();
        T.Check("every class the converter places exists by its Godot name", missing.Count == 0, string.Join(",", missing));
        var cannot = names.Where(n => n is not ("BaseButton" or "Range" or "Slider" or "ScrollBar" or "Separator" or "TreeItem") && !ClassDB.CanInstantiate(n)).ToList();
        T.Check("and the concrete ones can be made by name", cannot.Count == 0, string.Join(",", cannot));
        T.Check("the inheritance is Godot's", ClassDB.IsParentClass("HSlider", "Slider") && ClassDB.IsParentClass("HSlider", "Range") && ClassDB.IsParentClass("CheckBox", "Button") && ClassDB.IsParentClass("Button", "BaseButton") &&
                ClassDB.IsParentClass("OptionButton", "Button") && ClassDB.IsParentClass("ConfirmationDialog", "AcceptDialog") && ClassDB.IsParentClass("VScrollBar", "ScrollBar") && ClassDB.IsParentClass("HSplitContainer", "SplitContainer") &&
                ClassDB.IsParentClass("PopupMenu", "Popup") && ClassDB.IsParentClass("SpinBox", "Range") && ClassDB.IsParentClass("ProgressBar", "Range") && ClassDB.IsParentClass("HFlowContainer", "FlowContainer") && ClassDB.IsParentClass("ColorPickerButton", "Button"));
        T.Check("the theme looks a check box up as CheckBox, Button, BaseButton, Control", new CheckBox().ClassChain().SequenceEqual(new[] { "CheckBox", "Button", "BaseButton", "Control" }), string.Join(",", new CheckBox().ClassChain()));
        T.Check("the stand-in classes of the converter tests are gone: Button is the runtime's", typeof(Button).Assembly == typeof(Control).Assembly && typeof(HSlider).Assembly == typeof(Control).Assembly && typeof(TextureRect).Assembly == typeof(Control).Assembly);
        var b = new Button();
        T.Check("the members a converted script uses exist: pressed / toggled signals and names", Button.SignalName.Pressed == "pressed" && Button.SignalName.Toggled == "toggled" && HSlider.SignalName.ValueChanged == "value_changed" && LineEdit.SignalName.TextSubmitted == "text_submitted");
        b.Set("text", "Go");
        T.Check("and the Godot property names resolve by reflection", b.Text == "Go" && b.Get("text").AsString() == "Go");
        var slider = new HSlider();
        slider.Set("value", 33);
        slider.Set("min_value", 10);
        T.Check("a slider's set / get by name", slider.Value == 33 && slider.MinValue == 10 && slider.Get("value").AsDouble() == 33);
        var te = new TextureRect();
        te.Set("expand_mode", 1);
        T.Check("enums arrive as integers", te.ExpandMode == TextureRect.ExpandModeEnum.IgnoreSize);
    }

    // ---- the converter's output ----

    private static void ConverterOutput()
    {
        string exeName = "AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : "");
        string? exe = null;
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null && exe == null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", exeName);
            if (File.Exists(p)) exe = p;
        }
        if (exe == null) { Console.WriteLine("  SKIP: build/bin/AverAssetC was not found"); return; }
        string tmp = Path.Combine(Path.GetTempPath(), "aver-fabric-widget-conv-" + System.Environment.ProcessId);
        string data = Path.Combine(T.DataDir(), "WidgetGame");
        string src = Path.Combine(tmp, "WidgetGame");
        string outDir = Path.Combine(tmp, "out");
        try
        {
            foreach (string dir in Directory.GetDirectories(data, "*", SearchOption.AllDirectories)) Directory.CreateDirectory(dir.Replace(data, src));
            Directory.CreateDirectory(src);
            foreach (string file in Directory.GetFiles(data, "*", SearchOption.AllDirectories)) File.Copy(file, file.Replace(data, src), true);
            Directory.CreateDirectory(Path.Combine(src, "ui", "fonts"));
            byte[] font = { 0, 1, 0, 0, 7, 7 };
            File.WriteAllBytes(Path.Combine(src, "ui", "fonts", "Body-Regular.ttf"), font);
            File.WriteAllBytes(Path.Combine(src, "ui", "fonts", "Body-Bold.ttf"), font);
            File.WriteAllBytes(Path.Combine(src, "ui", "icon.png"), new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, (byte)'I', (byte)'H', (byte)'D', (byte)'R', 0, 0, 0, 12, 0, 0, 0, 10, 8, 6, 0, 0, 0, 0, 0, 0, 0 });
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(src); psi.ArgumentList.Add(outDir); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter runs on the WidgetGame fixture", proc.ExitCode == 0, "exit " + proc.ExitCode);
                if (proc.ExitCode != 0) return;
            }
            ConverterChecks(Path.Combine(outDir, "WidgetGame", "Content"));
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            ThemeDB.ResetForTests();
            try { Directory.Delete(tmp, true); } catch (IOException) { }
        }
    }

    private static void ConverterChecks(string contentRoot)
    {
        ResourceLoader.ContentRootOverride = contentRoot;
        ThemeDB.ResetForTests();
        Node main = ResourceLoader.Load<PackedScene>("res://main.tscn")!.Instantiate();
        Root.AddChild(main);
        Settle();
        T.Check("the level instantiates without an error", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));
        var root = (Control)main;

        var title = root.GetNode<RichTextLabel>("Title");
        T.Check("RichTextLabel: bbcode is on, the text is parsed, fit_content applied", title.BbcodeEnabled && title.FitContent && title.DumpDocument() == "bold[ 'Hello' ] ' ' meta[ 'door' ]", title.DumpDocument());
        var msg = root.GetNode<Label>("Msg");
        T.Check("Label: label_settings, autowrap_mode, alignment and max_lines_visible", msg.LabelSettings is { FontSize: 24, OutlineSize: 3 } && msg.AutowrapMode == TextServer.AutowrapMode.WordSmart &&
                msg.HorizontalAlignment == HorizontalAlignment.Center && msg.MaxLinesVisible == 2 && msg.LabelSettings.ShadowOffset == new Vector2(2, 3) && T.Near(msg.LabelSettings.LineSpacing, 5f));
        var start = root.GetNode<Button>("Start");
        var check = root.GetNode<CheckBox>("Check");
        T.Check("Button: toggle mode, and one ButtonGroup shared with the CheckBox (allow_unpress from the name)", start.ToggleMode && start.ButtonGroup != null && start.ButtonGroup == check.ButtonGroup && start.ButtonGroup.AllowUnpress);
        T.Check("its shortcut has the Ctrl key event", start.Shortcut is { } sc && sc.Events.Count == 1 && sc.Events[0].AsGodotObject() is InputEventKey { CtrlPressed: true });
        T.Check("its font override is a bold variation of the Body-Bold family", start.GetThemeFont("font") is FontVariation { } fv && fv.GetFontName() == "Body-Bold" && fv.GetFontStyleName() == "Bold");
        CanvasItem.DrawCommand text = Texts(start)[0];
        T.Check("and the draw command names the family and asks for bold", text.Family == "Body-Bold" && text.Flags == 1, $"{text.Family} {text.Flags}");
        var opt = root.GetNode<OptionButton>("Opt");
        T.Check("OptionButton: items from popup/item_N/..., the selection set before them", opt.ItemCount == 2 && opt.Selected == 1 && opt.Text == "Hard" && opt.GetItemId(1) == 20 && opt.GetItemText(0) == "Easy");
        var vol = root.GetNode<HSlider>("Vol");
        T.Check("HSlider: max 10, step 0.5, value 4, tick_count 3", vol.MaxValue == 10 && vol.Step == 0.5 && vol.Value == 4 && vol.TickCount == 3);
        var name = root.GetNode<LineEdit>("Name");
        T.Check("LineEdit: placeholder, max_length, secret", name.PlaceholderText == "your name" && name.MaxLength == 12 && name.Secret);
        var notes = root.GetNode<TextEdit>("Notes");
        T.Check("TextEdit: two lines and the wrap mode", notes.GetLineCount() == 2 && notes.GetLine(1) == "line two" && notes.WrapMode == TextEdit.LineWrappingMode.Boundary);
        var tabs = root.GetNode<TabContainer>("Tabs");
        var page1 = tabs.GetNode<Control>("Page1");
        var page2 = tabs.GetNode<Control>("Page2");
        T.Check("TabContainer: current_tab 1 shows the second page and hides the first", tabs.CurrentTab == 1 && page2.Visible && !page1.Visible && tabs.GetTabTitle(1) == "Page2");
        var tree = root.GetNode<Tree>("Tree");
        T.Check("Tree: two columns, titles, hidden root, row selection", tree.Columns == 2 && tree.ColumnTitlesVisible && tree.HideRoot && tree.SelectMode == Tree.SelectModeEnum.Row);
        var split = root.GetNode<HSplitContainer>("Split");
        T.Check("SplitContainer: the offset and the hidden divider", split.SplitOffset == 40 && split.DraggerVisibility == SplitContainer.DraggerVisibilityEnum.Hidden);
        var icon = root.GetNode<TextureRect>("Icon");
        T.Check("TextureRect: the texture (its size from the PNG header) and the modes", icon.Texture != null && icon.Texture.GetSize() == new Vector2(12, 10) && icon.ExpandMode == TextureRect.ExpandModeEnum.IgnoreSize &&
                icon.StretchMode == TextureRect.StretchModeEnum.KeepAspectCentered);
        var radial = root.GetNode<TextureProgressBar>("Radial");
        T.Check("TextureProgressBar: the radial mode and its angle are kept", radial.FillMode == TextureProgressBar.FillModeEnum.Clockwise && T.Near(radial.RadialInitialAngle, 90f) && T.Near((float)radial.Value, 50f));
        var prog = root.GetNode<ProgressBar>("Prog");
        T.Check("ProgressBar: value and show_percentage", prog.Value == 30 && !prog.ShowPercentage);
        var menu = root.GetNode<PopupMenu>("Menu");
        T.Check("PopupMenu: three items (a check item, a separator) and its flags; it starts hidden", menu.GetItemCount() == 3 && menu.GetItemText(0) == "New" && menu.GetItemId(0) == 1 && menu.IsItemCheckable(1) && menu.IsItemSeparator(2) &&
                !menu.HideOnItemSelection && !menu.Visible);
        var ask = root.GetNode<ConfirmationDialog>("Ask");
        T.Check("ConfirmationDialog: title, message and button texts", ask.Title == "Sure?" && ask.DialogText == "Really?" && ask.OkButtonText == "Yes" && ask.CancelButtonText == "No" && !ask.Visible);
        var code = root.GetNode<Control>("Code");
        T.Check("a CodeEdit is a Control carrier that names its class for theme lookups", code.GetThemeClassName() == "CodeEdit");
        var sys = root.GetNode<Label>("Sys");
        T.Check("a SystemFont keeps its names and draws with the default family", sys.GetThemeFont("font") is SystemFont { FontWeight: 700 } sf && sf.GetFontName() == "Arial" && Texts(sys)[0].Family == "");
        T.Check("the whole level lays out without an error", T.ErrorCount() == 0);
        // the fixture's two deliberate misses: an unknown Label property and the text of a CodeEdit carrier (kept as metadata, rated so)
        var missing = T.Logs.Where(l => (l.Message.Contains("has no property") || l.Message.Contains("setting '")) && !l.Message.Contains("bogus_label_property") && !l.Message.Contains("Control 'Code'")).Select(l => l.Message).ToList();
        T.Check("every other property the converter kept is a member of the runtime class (no 'has no property' warning)", missing.Count == 0, string.Join(" | ", missing));
        T.Check("the unknown Label property is kept as metadata", msg.HasMeta("bogus_label_property"));
        main.QueueFree();
        T.Frame(1.0 / 60.0, 1);
    }

    // ---- scene names ----

    private static void SceneNames()
    {
        var ob = new OptionButton();
        ob.ApplyValues(new (string, string)[] { ("selected", "1"), ("item_count", "3"), ("popup/item_0/text", "\"A\""), ("popup/item_0/id", "10"), ("popup/item_1/text", "\"B\""), ("popup/item_2/text", "\"C\""), ("popup/item_2/disabled", "true") });
        T.Check("OptionButton: items come from popup/item_N/..., the selection set before the items waits for them", ob.ItemCount == 3 && ob.GetItemText(0) == "A" && ob.GetItemId(0) == 10 && ob.Selected == 1 && ob.Text == "B" && ob.IsItemDisabled(2), $"{ob.ItemCount} {ob.Selected} '{ob.Text}'");
        var pm = new PopupMenu();
        pm.ApplyValues(new (string, string)[] { ("item_count", "2"), ("item_0/text", "\"New\""), ("item_0/id", "5"), ("item_1/text", "\"Auto\""), ("item_1/checkable", "1"), ("item_1/checked", "true"), ("item_1/separator", "false") });
        T.Check("PopupMenu: item_N/... fills the items", pm.GetItemCount() == 2 && pm.GetItemId(0) == 5 && pm.IsItemCheckable(1) && pm.IsItemChecked(1));
        var mb = new MenuButton();
        mb.ApplyValues(new (string, string)[] { ("text", "\"Menu\""), ("popup/item_0/text", "\"One\"") });
        T.Check("MenuButton keeps its items on its popup", mb.GetPopup().GetItemCount() == 1 && mb.Text == "Menu");
        var il = new ItemList();
        il.ApplyValues(new (string, string)[] { ("item_count", "2"), ("item_0/text", "\"x\""), ("item_1/text", "\"y\""), ("select_mode", "1") });
        T.Check("ItemList: item_N/text and an enum", il.ItemCount == 2 && il.GetItemText(1) == "y" && il.SelectMode == ItemList.SelectModeEnum.Multi);
        var bar = new TabBar();
        bar.ApplyValues(new (string, string)[] { ("tab_count", "2"), ("tab_0/title", "\"First\""), ("tab_1/title", "\"Second\""), ("tab_1/disabled", "true"), ("current_tab", "0") });
        T.Check("TabBar: tab_N/title and disabled", bar.TabCount == 2 && bar.GetTabTitle(1) == "Second" && bar.IsTabDisabled(1));
        var label = new Label();
        label.ApplyValues(new (string, string)[] { ("text", "\"Hi there\""), ("horizontal_alignment", "1"), ("autowrap_mode", "3"), ("text_overrun_behavior", "3"), ("uppercase", "true"), ("label_settings", "Object(LabelSettings, \"font_size\": 24, \"outline_size\": 2)") });
        T.Check("Label: enums as integers and a LabelSettings object", label.HorizontalAlignment == HorizontalAlignment.Center && label.AutowrapMode == TextServer.AutowrapMode.WordSmart &&
                label.TextOverrunBehavior == TextServer.OverrunBehavior.TrimEllipsis && label.Uppercase && label.LabelSettings is { FontSize: 24, OutlineSize: 2 });
        var b1 = new Button();
        var b2 = new Button();
        b1.ApplyValues(new (string, string)[] { ("toggle_mode", "true"), ("button_group", "Resource(\"ButtonGroups/Radio.ocbuttongroup\")") });
        b2.ApplyValues(new (string, string)[] { ("toggle_mode", "true"), ("button_group", "Resource(\"ButtonGroups/Radio.ocbuttongroup\")") });
        T.Check("a button group named by path is one shared group", b1.ButtonGroup != null && b1.ButtonGroup == b2.ButtonGroup && !b1.ButtonGroup.AllowUnpress);
        var b3 = new Button();
        b3.ApplyValues(new (string, string)[] { ("button_group", "Resource(\"ButtonGroups/Loose_unpress.ocbuttongroup\")") });
        T.Check("and the _unpress suffix carries allow_unpress", b3.ButtonGroup is { AllowUnpress: true });
        var rt = new RichTextLabel();
        rt.ApplyValues(new (string, string)[] { ("text", "\"[b]x[/b]\""), ("bbcode_enabled", "true") });
        T.Check("RichTextLabel: bbcode_enabled after text re-parses it", rt.DumpDocument() == "bold[ 'x' ]", rt.DumpDocument());
        var rt2 = new RichTextLabel();
        rt2.ApplyValues(new (string, string)[] { ("bbcode_enabled", "true"), ("text", "\"[b]x[/b]\"") });
        T.Check("and before it parses it once", rt2.DumpDocument() == "bold[ 'x' ]");
        var spin = new SpinBox();
        spin.ApplyValues(new (string, string)[] { ("min_value", "1"), ("max_value", "9"), ("value", "4"), ("prefix", "\"x\""), ("update_on_text_changed", "true") });
        T.Check("SpinBox: range values and the prefix", spin.MinValue == 1 && spin.MaxValue == 9 && spin.Value == 4 && spin.Prefix == "x" && spin.UpdateOnTextChanged);
        var ctrl = new Button();
        ctrl.ApplyValues(new (string, string)[] { ("theme_override_fonts/font", "Resource(\"Fonts/ui.ttf\")"), ("theme_override_font_sizes/font_size", "30"), ("text", "\"Hi\"") });
        T.Check("a font override by path arrives as a font of that family", ctrl.GetThemeFont("font") is { } f && f.GetFontName() == "ui" && ctrl.GetThemeFontSize("font_size") == 30);
        var variation = new Label();
        variation.ApplyValues(new (string, string)[] { ("theme_override_fonts/font", "Object(FontVariation, \"base_font\": Resource(\"Fonts/ui.ttf\"), \"variation_embolden\": 1.0)") });
        T.Check("a FontVariation object keeps its base font and asks for bold", variation.GetThemeFont("font") is FontVariation { VariationEmbolden: 1f } v && v.GetFontName() == "ui" && v.GetFontStyleName() == "Bold");
        var shortcutButton = new Button();
        shortcutButton.ApplyValues(new (string, string)[] { ("shortcut", "Object(Shortcut, \"events\": [Object(InputEventKey, \"keycode\": 4194332, \"ctrl_pressed\": true)])") });
        T.Check("a Shortcut with an input event arrives whole", shortcutButton.Shortcut is { } sc && sc.Events.Count == 1 && sc.Events[0].AsGodotObject() is InputEventKey { CtrlPressed: true });
    }
}
