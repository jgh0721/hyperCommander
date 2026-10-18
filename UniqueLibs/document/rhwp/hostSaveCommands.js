// Preserve Studio command labels, menus and shortcuts, but never its browser
// save implementations. Native code owns permissions, dialogs and atomic writes.
export function InstallHostSaveCommands(Registry, FileCommands, Events) {
    let Listener, Enabled = false, Saving = false, Sending = false;
    const CanExecute = Context => Enabled && !Saving && !Sending && !!Listener && Context.hasDocument;
    const Refresh = () => Events.emit('command-state-changed');
    for (const [Id, Action] of [['file:save','save'], ['file:save-as','saveAs'], ['file:print','print']]) {
        const Original = FileCommands.find(Command => Command.id === Id);
        if (!Original) throw Error('host-save-command-missing');
        Registry.register({...Original, canExecute: CanExecute, execute: async () => {
            if (!Enabled || Saving || Sending || !Listener) return;
            Sending = true; Refresh();
            try { await Listener(Action); }
            finally { Sending = false; Refresh(); }
        }});
    }
    return {
        setEnabled(Value) { Enabled = !!Value; Refresh(); },
        setSaving(Value) { Saving = !!Value; Refresh(); },
        onAction(Value) { Listener = Value; Refresh(); }
    };
}
