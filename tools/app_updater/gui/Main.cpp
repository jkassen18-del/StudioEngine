// App Updater - desktop app (JUCE). Pick the installed app, pick the new exe, press Update.
//
// Command line (also used when restarting as administrator):
//   "App Updater.exe" [installed.exe] [new.exe] [--auto] [--no-backup] [--close] [--relaunch]
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Platform.h"
#include "../UpdaterCore.h"

#include <atomic>
#include <thread>

namespace
{
namespace fs = std::filesystem;

fs::path toPath (const juce::File& f) { return fs::path (f.getFullPathName().toWideCharPointer()); }
fs::path toPath (const juce::String& s) { return fs::path (s.toWideCharPointer()); }
juce::String toString (const fs::path& p) { return juce::String (p.wstring().c_str()); }

juce::String prettyStamp (const std::string& s) // YYYYMMDD-HHMMSS -> 2026-11-01 09:18:53
{
    if (s.size() < 15)
        return s;
    return juce::String (s.substr (0, 4) + "-" + s.substr (4, 2) + "-" + s.substr (6, 2) + "  "
                         + s.substr (9, 2) + ":" + s.substr (11, 2) + ":" + s.substr (13, 2));
}

const juce::Colour bg { 0xff1d2026 }, panel { 0xff272b33 }, accent { 0xff4c9aff }, textDim { 0xff9aa3b2 };

// =============================================================================================
class FileRow : public juce::Component
{
public:
    std::function<void()> onChange;

    FileRow (const juce::String& title, const juce::String& hint) : heading (title)
    {
        addAndMakeVisible (path);
        path.setTextToShowWhenEmpty (hint, textDim);
        path.onTextChange = [this] { refreshInfo(); if (onChange) onChange(); };
        path.setColour (juce::TextEditor::backgroundColourId, panel);
        path.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a404b));

        addAndMakeVisible (browse);
        browse.onClick = [this] { choose(); };

        addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, textDim);
        info.setFont (juce::FontOptions (13.0f));
    }

    juce::String getText() const { return path.getText().trim().unquoted(); }
    void setFile (const juce::String& f) { path.setText (f, juce::sendNotification); }

    void refreshInfo()
    {
        const juce::File f (getText());
        if (getText().isEmpty() || ! juce::File::isAbsolutePath (getText()))
        {
            info.setText ({}, juce::dontSendNotification);
            return;
        }
        if (! f.existsAsFile())
        {
            info.setText ("File not found", juce::dontSendNotification);
            return;
        }
        const auto p = toPath (f);
        juce::String s = upd::toString (upd::detectExecutable (p));
        const auto v = upd::platform::fileVersion (p);
        if (! v.empty())
            s << "  |  version " << v;
        s << "  |  " << upd::humanSize ((std::uintmax_t) f.getSize())
          << "  |  modified " << f.getLastModificationTime().toString (true, true, false);
        if (! upd::platform::findProcesses (p).empty())
            s << "  |  RUNNING";
        info.setText (s, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (heading, getLocalBounds().removeFromTop (22), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (24);
        auto line = r.removeFromTop (30);
        browse.setBounds (line.removeFromRight (100));
        line.removeFromRight (8);
        path.setBounds (line);
        info.setBounds (r.removeFromTop (22));
    }

private:
    void choose()
    {
        const auto start = juce::File (getText()).existsAsFile() ? juce::File (getText())
                                                                  : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
       #if JUCE_WINDOWS
        const juce::String filter = "*.exe";
       #else
        const juce::String filter = "*";
       #endif
        chooser = std::make_unique<juce::FileChooser> (heading, start, filter);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File())
                                      setFile (fc.getResult().getFullPathName());
                              });
    }

    juce::String heading;
    juce::TextEditor path;
    juce::TextButton browse { "Browse..." };
    juce::Label info;
    std::unique_ptr<juce::FileChooser> chooser;
};

// =============================================================================================
class UpdaterComponent : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    UpdaterComponent()
    {
        juce::PropertiesFile::Options po;
        po.applicationName = "App Updater";
        po.filenameSuffix = ".settings";
        po.osxLibrarySubFolder = "Application Support";
        props = std::make_unique<juce::PropertiesFile> (po);

        addAndMakeVisible (recentLabel);
        recentLabel.setText ("Recent apps", juce::dontSendNotification);
        recentLabel.setColour (juce::Label::textColourId, textDim);
        addAndMakeVisible (recent);
        recent.setTextWhenNothingSelected ("Pick an app you updated before...");
        recent.setTextWhenNoChoicesAvailable ("(no apps updated yet)");
        recent.onChange = [this] {
            if (recent.getSelectedId() > 0)
                installed.setFile (recentApps[recent.getSelectedId() - 1]);
        };
        loadRecent();

        addAndMakeVisible (installed);
        addAndMakeVisible (incoming);
        installed.onChange = incoming.onChange = [this] { updateButtons(); };

        for (auto* t : { &keepBackup, &closeApp, &relaunch })
            addAndMakeVisible (t);
        keepBackup.setToggleState (props->getBoolValue ("keepBackup", true), juce::dontSendNotification);
        closeApp.setToggleState (props->getBoolValue ("closeApp", true), juce::dontSendNotification);
        relaunch.setToggleState (props->getBoolValue ("relaunch", false), juce::dontSendNotification);
        keepBackup.setTooltip ("Saves a copy of the current version first, so you can go back to it.");
        closeApp.setTooltip ("If the app is open, it is asked to close (and forced after 10 seconds).");

        addAndMakeVisible (updateBtn);
        updateBtn.setColour (juce::TextButton::buttonColourId, accent);
        updateBtn.onClick = [this] { startUpdate(); };

        addAndMakeVisible (restoreBtn);
        restoreBtn.onClick = [this] { showRestoreMenu(); };

        addAndMakeVisible (backupsBtn);
        backupsBtn.onClick = [this] {
            const juce::File dir (toString (upd::backupDirFor (toPath (installed.getText()), {})));
            (dir.isDirectory() ? dir : juce::File (toString (upd::defaultBackupRoot()))).startAsProcess();
        };

        addAndMakeVisible (log);
        log.setMultiLine (true);
        log.setReadOnly (true);
        log.setScrollbarsShown (true);
        log.setCaretVisible (false);
        log.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        log.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff15171b));
        log.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a404b));
        addLog ("Drop files here, or use Browse. The installed app is replaced by the new file;"
                " its name and location stay the same.");

        setSize (720, 600);
        updateButtons();
        startTimer (2000); // keep the "RUNNING" indicator fresh
    }

    ~UpdaterComponent() override
    {
        stopTimer();
        if (worker.joinable())
            worker.join();
        props->setValue ("keepBackup", keepBackup.getToggleState());
        props->setValue ("closeApp", closeApp.getToggleState());
        props->setValue ("relaunch", relaunch.getToggleState());
        props->saveIfNeeded();
    }

    void applyCommandLine (const juce::StringArray& args)
    {
        juce::StringArray files;
        bool autoRun = false;
        for (auto& a : args)
        {
            if (a == "--auto") autoRun = true;
            else if (a == "--no-backup") keepBackup.setToggleState (false, juce::dontSendNotification);
            else if (a == "--close") closeApp.setToggleState (true, juce::dontSendNotification);
            else if (a == "--relaunch") relaunch.setToggleState (true, juce::dontSendNotification);
            else if (a.isNotEmpty() && ! a.startsWith ("-")) files.add (a.unquoted());
        }
        if (files.size() > 0) installed.setFile (files[0]);
        if (files.size() > 1) incoming.setFile (files[1]);
        if (autoRun && files.size() > 1)
            juce::MessageManager::callAsync ([sp = SafePointer (this)] { if (sp) sp->runUpdate(); });
    }

    // ---- drag & drop ------------------------------------------------------------------------
    bool isInterestedInFileDrag (const juce::StringArray&) override { return ! busy; }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        if (files.size() >= 2)
        {
            installed.setFile (files[0]);
            incoming.setFile (files[1]);
            addLog ("Two files dropped: check which one is the installed app before updating.");
            return;
        }
        const auto p = juce::Point<int> (x, y);
        if (installed.getBounds().contains (p) || (! incoming.getBounds().contains (p) && installed.getText().isEmpty()))
            installed.setFile (files[0]);
        else
            incoming.setFile (files[0]);
    }

    // ---- layout -----------------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        g.fillAll (bg);
        auto top = getLocalBounds().reduced (20).removeFromTop (40);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawText ("App Updater", top.removeFromTop (28), juce::Justification::centredLeft);
        g.setColour (textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Replace an installed program with its new version, with a backup you can roll back to.",
                    top, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        r.removeFromTop (48);
        auto rec = r.removeFromTop (28);
        recentLabel.setBounds (rec.removeFromLeft (90));
        recent.setBounds (rec);
        r.removeFromTop (10);
        installed.setBounds (r.removeFromTop (78));
        r.removeFromTop (6);
        incoming.setBounds (r.removeFromTop (78));
        r.removeFromTop (8);
        auto opts = r.removeFromTop (26);
        keepBackup.setBounds (opts.removeFromLeft (190));
        closeApp.setBounds (opts.removeFromLeft (230));
        relaunch.setBounds (opts);
        r.removeFromTop (12);
        auto btns = r.removeFromTop (36);
        updateBtn.setBounds (btns.removeFromLeft (180));
        btns.removeFromLeft (10);
        restoreBtn.setBounds (btns.removeFromLeft (220));
        btns.removeFromLeft (10);
        backupsBtn.setBounds (btns.removeFromLeft (170));
        r.removeFromTop (12);
        log.setBounds (r);
    }

private:
    void timerCallback() override
    {
        if (! busy)
        {
            installed.refreshInfo();
            incoming.refreshInfo();
        }
    }

    void updateButtons()
    {
        const bool haveTarget = juce::File::isAbsolutePath (installed.getText()) && juce::File (installed.getText()).existsAsFile();
        const bool haveNew = juce::File::isAbsolutePath (incoming.getText()) && juce::File (incoming.getText()).existsAsFile();
        updateBtn.setEnabled (! busy && haveTarget && haveNew);
        restoreBtn.setEnabled (! busy && haveTarget);
        for (juce::Component* c : { (juce::Component*) &installed, (juce::Component*) &incoming, (juce::Component*) &recent,
                                    (juce::Component*) &keepBackup, (juce::Component*) &closeApp, (juce::Component*) &relaunch })
            c->setEnabled (! busy);
        updateBtn.setButtonText (busy ? "Working..." : "Update Now");
    }

    void addLog (const juce::String& s)
    {
        log.moveCaretToEnd();
        log.insertTextAtCaret ((log.isEmpty() ? "" : "\n") + s);
    }

    upd::Options makeOptions()
    {
        upd::Options o;
        o.keepBackup = keepBackup.getToggleState();
        o.closeRunning = closeApp.getToggleState();
        o.relaunch = relaunch.getToggleState();
        o.log = [sp = SafePointer (this)] (const std::string& s) {
            juce::MessageManager::callAsync ([sp, msg = juce::String (s)] { if (sp) sp->addLog (msg); });
        };
        return o;
    }

    void startUpdate()
    {
        const auto t = toPath (installed.getText());
        const auto n = toPath (incoming.getText());
        const auto oldV = upd::platform::fileVersion (t), newV = upd::platform::fileVersion (n);

        juce::String msg;
        msg << "Replace\n    " << installed.getText() << "\nwith\n    " << incoming.getText() << " ?";
        if (! oldV.empty() && ! newV.empty())
            msg << "\n\nVersion " << oldV << "  ->  " << newV;
        if (! oldV.empty() && ! newV.empty() && juce::String (newV).compareNatural (juce::String (oldV)) < 0)
            msg << "\n\nWarning: the new file has an OLDER version number.";
        if (t.filename() != n.filename())
            msg << "\n\nNote: the file names differ. The installed app keeps its name ("
                << toString (t.filename()) << ").";
        if (! keepBackup.getToggleState())
            msg << "\n\nNo backup will be kept, so this cannot be undone.";

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Update app", msg, "Update", "Cancel", this,
            juce::ModalCallbackFunction::create ([sp = SafePointer (this)] (int result) {
                if (sp && result == 1)
                    sp->runUpdate();
            }));
    }

    void runUpdate()
    {
        lastJobWasRestore = false;
        runInBackground ([t = toPath (installed.getText()), n = toPath (incoming.getText())] (const upd::Options& o) {
            return upd::update (t, n, o);
        });
    }

    void showRestoreMenu()
    {
        const auto t = toPath (installed.getText());
        const auto backups = upd::listBackups (t, {});
        if (backups.empty())
        {
            addLog ("There are no backups of " + installed.getText() + " yet. One is made every time you update it.");
            return;
        }
        juce::PopupMenu m;
        m.addSectionHeader ("Restore which version?");
        for (size_t i = 0; i < backups.size(); ++i)
        {
            juce::String item = prettyStamp (backups[i].timestamp);
            if (! backups[i].version.empty())
                item << "   v" << backups[i].version;
            item << "   (" << upd::humanSize (backups[i].size) << ")";
            if (i == 0)
                item << "   - previous version";
            m.addItem ((int) i + 1, item);
        }
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (restoreBtn),
                         [sp = SafePointer (this), t, backups] (int choice) {
                             if (! sp || choice <= 0)
                                 return;
                             const auto b = backups[(size_t) choice - 1].file;
                             sp->lastJobWasRestore = true;
                             sp->runInBackground ([t, b] (const upd::Options& o) { return upd::restore (t, b, o); });
                         });
    }

    void runInBackground (std::function<upd::Result (const upd::Options&)> job)
    {
        if (busy)
            return;
        if (worker.joinable())
            worker.join();
        busy = true;
        updateButtons();
        addLog ("----------------------------------------");
        auto opts = makeOptions();
        worker = std::thread ([job, opts, sp = SafePointer (this)] {
            auto r = job (opts);
            juce::MessageManager::callAsync ([sp, r] { if (sp) sp->finished (r); });
        });
    }

    void finished (const upd::Result& r)
    {
        busy = false;
        updateButtons();
        installed.refreshInfo();
        if (r.ok)
        {
            rememberRecent (installed.getText());
            if (r.changed)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Done", juce::String (r.message), {}, this);
            return;
        }
        if (r.needsElevation && ! lastJobWasRestore)
        {
           #if JUCE_WINDOWS
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Administrator rights needed",
                "This app is installed in a protected folder. Restart App Updater as administrator and finish the update?",
                "Restart as admin", "Cancel", this,
                juce::ModalCallbackFunction::create ([sp = SafePointer (this)] (int res) { if (sp && res == 1) sp->restartElevated(); }));
            return;
           #endif
        }
        juce::String msg (r.message);
        if (r.needsElevation)
            msg << "\n\nRight-click App Updater and choose \"Run as administrator\", then try again.";
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update failed", msg, {}, this);
    }

    void restartElevated()
    {
        std::vector<std::wstring> args { installed.getText().toWideCharPointer(), incoming.getText().toWideCharPointer(), L"--auto" };
        if (! keepBackup.getToggleState()) args.push_back (L"--no-backup");
        if (closeApp.getToggleState()) args.push_back (L"--close");
        if (relaunch.getToggleState()) args.push_back (L"--relaunch");
        if (upd::platform::relaunchElevated (upd::platform::currentExecutable(), args))
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        else
            addLog ("Administrator rights were not granted.");
    }

    void loadRecent()
    {
        recentApps.clear();
        recentApps.addTokens (props->getValue ("recentApps"), "|", "");
        recentApps.removeEmptyStrings();
        recent.clear (juce::dontSendNotification);
        for (int i = 0; i < recentApps.size(); ++i)
            recent.addItem (recentApps[i], i + 1);
    }

    void rememberRecent (const juce::String& f)
    {
        recentApps.removeString (f, true);
        recentApps.insert (0, f);
        while (recentApps.size() > 10)
            recentApps.remove (recentApps.size() - 1);
        props->setValue ("recentApps", recentApps.joinIntoString ("|"));
        props->saveIfNeeded();
        loadRecent();
    }

    std::unique_ptr<juce::PropertiesFile> props;
    juce::StringArray recentApps;
    juce::Label recentLabel;
    juce::ComboBox recent;
    FileRow installed { "1.  Installed app", "The program you want to update, e.g. C:\\Program Files\\MyApp\\MyApp.exe" };
    FileRow incoming { "2.  New version", "The new .exe you downloaded or built" };
    juce::ToggleButton keepBackup { "Keep a backup" }, closeApp { "Close the app if it's running" }, relaunch { "Start it when done" };
    juce::TextButton updateBtn { "Update Now" }, restoreBtn { "Restore a Previous Version..." }, backupsBtn { "Open Backups Folder" };
    juce::TextEditor log;
    juce::TooltipWindow tooltips { this };
    std::thread worker;
    std::atomic<bool> busy { false };
    bool lastJobWasRestore = false;
};

// =============================================================================================
class AppUpdaterApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "App Updater"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        laf.setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);
        window = std::make_unique<MainWindow>();
        if (auto* c = dynamic_cast<UpdaterComponent*> (window->getContentComponent()))
            c->applyCommandLine (getCommandLineParameterArray());
    }

    void shutdown() override
    {
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    struct MainWindow : public juce::DocumentWindow
    {
        MainWindow() : DocumentWindow ("App Updater", bg, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new UpdaterComponent(), true);
            setResizable (true, false);
            setResizeLimits (620, 520, 1400, 1200);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    juce::LookAndFeel_V4 laf;
    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION (AppUpdaterApplication)
