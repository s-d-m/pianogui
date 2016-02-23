#include <signal.h>
#include <iostream>
#include <QFileDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include "mainwindow.hh"
#include "ui_mainwindow.hh"

// Global variables to "share" state between the signal handler and
// the main event loop.  Only these two pieces should be allowed to
// use these global variables.  To avoid any other piece piece of code
// from using it, the declaration is not written on a header file on
// purpose.
extern volatile sig_atomic_t pause_requested;
extern volatile sig_atomic_t continue_requested;
extern volatile sig_atomic_t exit_requested;


void MainWindow::look_for_signals_change()
{

  if (exit_requested)
  {
    close();
  }

  if (pause_requested)
  {
    pause_requested = 0;
    is_in_pause = true;
  }

  if (continue_requested)
  {
    continue_requested = 0;
    is_in_pause = false;
  }

}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
  const auto pressed_key = event->key();

  if ((pressed_key == Qt::Key_Space) or
      (pressed_key == Qt::Key_P) or
      (pressed_key == Qt::Key_Pause))
  {
    // toggle play pause
    is_in_pause = not is_in_pause;
  }
}

void MainWindow::process_keyboard_event(const music_event& keys_event)
{
  update_keyboard(keys_event, this->keyboard);
  draw_keyboard(*(this->scene), this->keyboard);
  this->update();

  for (const auto& message : keys_event.midi_messages)
  {
    auto tmp = message; // can't use message directly since message is const and
			// sendMessage doesn't take a const vector
    sound_player.sendMessage(&tmp);

    // could use the following to cast the const away: but since there is no
    // guarantee that the libRtMidi doesn't modify the data ... (I know the I
    // can read the code)
    //
    //sound_player.sendMessage(const_cast<midi_message*>(&message));
  }
}

void MainWindow::song_event_loop()
{
  if (is_in_pause.load())
  {
    QTimer::singleShot(100, this, SLOT(song_event_loop()));
    return;
  }

  if (song_pos == INVALID_SONG_POS)
  {
    QTimer::singleShot(100, this, SLOT(song_event_loop()));
    return;
  }

  if ((song_pos != 0) and (song_pos >= song.size()))
  {
    throw std::runtime_error("Invalid song position found");
  }

  process_keyboard_event( song[song_pos] );

  if (song_pos + 1 == song.size())
  {
    // song is finished
    stop_song();
    QTimer::singleShot(100, this, SLOT(song_event_loop()));
  }
  else
  {
    // call this function back for the next event
    song_pos++;
    QTimer::singleShot( static_cast<int>((song[song_pos].time - song[song_pos - 1].time) / 1'000'000), this, SLOT(song_event_loop()) );
  }

}

void MainWindow::stop_song()
{
  this->is_in_pause = true;

  {
    // just close the output ports, and reopens it. This avoids getting a 'buzzing' noise
    // being played continuously through the speakers. It also avoids the need to create a
    // all-keys-up vector to get played through the MIDI output to release the piano keys.
    sound_player.closePort();

    const auto nb_ports = sound_player.getPortCount();
    for (unsigned int i = 0; i < nb_ports; ++i)
    {
      const auto port_name = sound_player.getPortName(i);
      if (port_name == selected_output_port)
      {
	sound_player.openPort(i);
      }
    }
  }

  // reinitialise the song field
  this->song.clear();
  this->song_pos = INVALID_SONG_POS;

  // reset all keys to up on the keyboard (doesn't play key_released events).
  reset_color(keyboard);
}

void MainWindow::open_file(const std::string& filename)
{
  try
  {
    stop_song();
    const auto midi_events = get_midi_events(filename);
    const auto keyboard_events = get_key_events(midi_events);
    this->song = group_events_by_time(midi_events, keyboard_events);
    this->song_pos = 0;
    sound_listener.closePort();
    this->selected_input.clear();
    this->is_in_pause = false;
  }
  catch (std::exception& e)
  {
    const auto err_msg = e.what();
    QMessageBox::critical(this, tr("Failed to open file."),
			  err_msg,
			  QMessageBox::Ok,
			  QMessageBox::Ok);
  }
}

void MainWindow::open_file()
{
  QStringList filters;
  filters << "Midi files (*.midi *.mid)"
  	  << "Any files (*)";

  QFileDialog dialog;
  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setViewMode(QFileDialog::List);
  dialog.setNameFilters(filters);

  const auto dialog_ret = dialog.exec();
  if (dialog_ret == QDialog::Accepted)
  {
    const auto files = dialog.selectedFiles();
    if (files.length() == 1)
    {
      const auto file = files[0];
      const auto filename = file.toStdString();

      open_file(filename);
    }
  }
}

void MainWindow::set_output_port(const unsigned int i)
{
  try
  {
    sound_player.closePort();
    sound_player.openPort(i);
    const auto port_name = sound_player.getPortName(i);
    this->selected_output_port = port_name;
    this->update_output_ports();
  }
  catch (std::exception& e)
  {
    const auto err_msg = e.what();
    QMessageBox::critical(this, tr("Failed to change the output port."),
			  err_msg,
			  QMessageBox::Ok,
			  QMessageBox::Ok);

    // failed to change port, clear the selected item. There is no need to set the button
    // to unchecked as the menu is automatically closed after selecting an item, and the
    // entries are regenerated when the menu is opened again.
    this->selected_output_port.clear();

    // make sure to close all output ports
    this->sound_player.closePort();
  }
}

void MainWindow::output_port_change()
{
  // find out which output port is now checked.
  auto menu_bar = this->menuBar();
  if (menu_bar == nullptr)
  {
    std::cerr << "Error: couldn't find the menu bar\n";
    return;
  }

  auto menu_output_port = menu_bar->findChild<QMenu*>("menuOutput_port",
						      Qt::FindDirectChildrenOnly);
  if (menu_output_port == nullptr)
  {
    std::cerr << "Error: couldn't find the output ports menu\n";
    return;
  }

  auto button_list = menu_output_port->findChildren<QAction*>(QString(), Qt::FindDirectChildrenOnly);
  for (auto& button : button_list)
  {
    if (button->isChecked())
    {
      this->selected_output_port = button->text().toStdString();
      const auto nb_ports = sound_player.getPortCount();
      for (unsigned int i = 0; i < nb_ports; ++i)
      {
	const auto port_name = sound_player.getPortName(i);
	if (port_name == selected_output_port)
	{
	  sound_player.closePort();
	  sound_player.openPort(i);
	}
      }
    }
  }
}

void MainWindow::update_output_ports()
{
  // find out which output port is now checked.
  auto menu_bar = this->menuBar();
  if (menu_bar == nullptr)
  {
    std::cerr << "Error: couldn't find the menu bar\n";
    return;
  }

  auto menu_output_port = menu_bar->findChild<QMenu*>("menuOutput_port",
						      Qt::FindDirectChildrenOnly);
  if (menu_output_port == nullptr)
  {
    std::cerr << "Error: couldn't find the output ports menu\n";
    return;
  }

  const auto nb_ports = sound_player.getPortCount();
  if (nb_ports == 0)
  {
    std::cerr << "Sorry: can't populate menu, no output midi port found\n";
    return;
  }

  // remove all the children!
  menu_output_port->clear();

  // find the action group.
  auto action_group = menu_output_port->findChild<QActionGroup*>("",
								 Qt::FindDirectChildrenOnly);
  if (action_group == nullptr)
  {
    action_group = new QActionGroup( menu_output_port );
  }

  for (unsigned int i = 0; i < nb_ports; ++i)
  {
    const auto port_name = sound_player.getPortName(i);
    const auto label = QString::fromStdString( port_name );
    auto button = menu_output_port->addAction(label);
    button->setCheckable(true);
    const auto select_this_port = ( port_name == selected_output_port );
    button->setChecked( select_this_port );

    button->setActionGroup( action_group );
    connect(button, SIGNAL(triggered()), this, SLOT(output_port_change()));
  }
}

void MainWindow::handle_input_midi(std::vector<unsigned char> message)
{
  std::vector<midi_message> tmp;
  tmp.push_back(message);

  const music_event event ( /* time */ 0,
			    /* midi_message */ tmp,
			    /* key events */ midi_to_key_events(message) );

  this->process_keyboard_event(event);
}

void MainWindow::on_midi_input(double timestamp __attribute__((unused)), std::vector<unsigned char> *message, void* param)
{
  if (message == nullptr)
  {
    throw std::invalid_argument("Error, invalid input message");
  }

  if (param == nullptr)
  {
    throw std::invalid_argument("Error, argument for input listener");
  }

  auto window = static_cast<class MainWindow*>(param);
  emit window->midi_message_received(*message);
}

void MainWindow::set_input_port(unsigned int i)
{
  const auto port_name = sound_listener.getPortName(i);
  this->selected_input = port_name;
  sound_listener.closePort();
  sound_listener.openPort(i);
  sound_listener.setCallback(&MainWindow::on_midi_input, this);
}


void MainWindow::input_change()
{
  // find out which menu item has been clicked.
  const auto menu_bar = this->menuBar();
  if (menu_bar == nullptr)
  {
    std::cerr << "Error: couldn't find the menu bar\n";
    return;
  }

  const auto menu_input = menu_bar->findChild<QMenu*>("menuInput",
						      Qt::FindDirectChildrenOnly);
  if (menu_input == nullptr)
  {
    std::cerr << "Error: couldn't find the output ports menu\n";
    return;
  }

  const auto button_list = menu_input->findChildren<QAction*>(QString(), Qt::FindDirectChildrenOnly);

  try
  {
    for (const auto& button : button_list)
    {
      if (button->isChecked())
      {
	this->stop_song();
	this->selected_input = button->text().toStdString();
	const auto nb_ports = sound_listener.getPortCount();
	for (unsigned int i = 0; i < nb_ports; ++i)
	{
	  const auto port_name = sound_listener.getPortName(i);
	  if (port_name == selected_input)
	  {
	    this->set_input_port(i);
	  }
	}
      }
    }
  }
  catch (std::exception& e)
  {
    const auto err_msg = e.what();
    QMessageBox::critical(this, tr("Failed to change the input."),
			  err_msg,
			  QMessageBox::Ok,
			  QMessageBox::Ok);

    // failed to change port, clear the selected item. There is no need to set the button
    // to unchecked as the menu is automatically closed after selecting an item, and the
    // entries are regenerated when the menu is opened again.
    this->selected_input.clear();

    // make sure to close all inputs ports
    sound_listener.closePort();
  }
}

void MainWindow::update_input_entries()
{
  // find out which input is currently selected
  auto menu_bar = this->menuBar();
  if (menu_bar == nullptr)
  {
    std::cerr << "Error: couldn't find the menu bar\n";
    return;
  }

  auto menu_input = menu_bar->findChild<QMenu*>("menuInput",
						Qt::FindDirectChildrenOnly);
  if (menu_input == nullptr)
  {
    std::cerr << "Error: couldn't find the input menu\n";
    return;
  }

  // remove all the children!
  menu_input->clear();

  // find the action group.
  auto action_group = menu_input->findChild<QActionGroup*>("",
							   Qt::FindDirectChildrenOnly);
  if (action_group == nullptr)
  {
    action_group = new QActionGroup( menu_input );
  }

  {
    // add the file entry in the input menu.
    std::string label { "select file" };
    const auto Qlabel = QString::fromStdString( label );
    auto button = menu_input->addAction(Qlabel);

    button->setActionGroup(action_group);
    connect(button, SIGNAL(triggered()), this, SLOT(open_file()));
  }

  {
    // Add one entry per input midi port
    const auto nb_ports = sound_listener.getPortCount();
    for (unsigned int i = 0; i < nb_ports; ++i)
    {
      const auto port_name = sound_listener.getPortName(i);
      const auto label = QString::fromStdString( port_name );
      auto button = menu_input->addAction(label);
      button->setCheckable(true);
      const auto select_this_port = ( port_name == selected_input );
      button->setChecked( select_this_port );

      button->setActionGroup( action_group );
      connect(button, SIGNAL(triggered()), this, SLOT(input_change()));
    }
  }
}


#pragma GCC diagnostic push
#if !defined(__clang__)
  #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant" // Qt is not effective-C++ friendy
#endif

MainWindow::MainWindow(QWidget *parent) :
  QMainWindow(parent),
  ui(new Ui::MainWindow),
  scene(new QGraphicsScene(this)),
  keyboard(),
  signal_checker_timer(),
  song(),
  sound_player(RtMidi::LINUX_ALSA),
  sound_listener(RtMidi::LINUX_ALSA),
  is_in_pause()
{
  atomic_init(&is_in_pause, true);

  ui->setupUi(this);
  ui->keyboard->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
  ui->keyboard->setScene(scene);
  draw_keyboard(*scene, this->keyboard);

  connect(&signal_checker_timer, SIGNAL(timeout()), this, SLOT(look_for_signals_change()));
  signal_checker_timer.start(100 /* ms */);

  {
    // automatically open an output midi port if possible
    const auto nb_ports = sound_player.getPortCount();
    if (nb_ports == 0)
    {
      std::cerr << "Sorry: no output midi port found\n";
    }
    else
    {
      // automatically open an output midi port.
      const unsigned int port_to_use = (nb_ports == 1) ? 0 : 1;
      sound_player.openPort( port_to_use );
      this->selected_output_port = sound_player.getPortName( port_to_use );
    }
  }

  {
    // setting up the signal on_output_ports_menu_clicked->update_outputs_ports.
    // update_outputs_ports is the function that fills up the menu entries with all the ports
    // output ports available.
    auto menu_bar = this->menuBar();
    auto menu_output_port = menu_bar->findChild<QMenu*>("menuOutput_port",
							Qt::FindDirectChildrenOnly);
    if (menu_output_port == nullptr)
    {
      std::cerr << "Error: couldn't find the output port menu\n";
    }
    else
    {
      connect(menu_output_port, SIGNAL(aboutToShow()), this, SLOT(update_output_ports()));
    }
  }

  {
    // setting up the signal on_input_menu_clicked->update_input_entries. Update_input_entries fills up
    // the input menu with the available entries
    // an important difference between the input_menu and the output_menu is that the input is not necessary
    // a midi input port. It can be a file. And therefore, if there is no midi inputs detected, it's not a problem.
    // and also, there is no need to automatically select an input port.
    auto menu_bar = this->menuBar();
    auto menu_input = menu_bar->findChild<QMenu*>("menuInput",
						  Qt::FindDirectChildrenOnly);
    if (menu_input == nullptr)
    {
      std::cerr << "Error: couldn't find the input menu\n";
    }
    else
    {
      connect(menu_input, SIGNAL(aboutToShow()), this, SLOT(update_input_entries()));
    }
  }

  {
    qRegisterMetaType<std::vector<unsigned char>>("std::vector<unsigned char>");
    connect(this, SIGNAL(midi_message_received(std::vector<unsigned char>)), this, SLOT(handle_input_midi(std::vector<unsigned char>)));
  }

  {
    song_event_loop();
  }
}

#pragma GCC diagnostic pop

MainWindow::~MainWindow()
{
  delete ui;
  sound_player.closePort();
}
