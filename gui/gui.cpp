#
/*
    *    Copyright (C) 2013, 2014, 2015
    *    Jan van Katwijk (J.vanKatwijk@gmail.com)
    *    Lazy Chair Programming
    *
    *    This file is part of the  SDR-J (JSDR).
    *    Many of the ideas as implemented in SDR-J are derived from
    *    other work, made available through the GNU general Public License.
    *    All copyrights of the original authors are acknowledged.
    *
    *    SDR-J is free software; you can redistribute it and/or modify
    *    it under the terms of the GNU General Public License as published by
    *    the Free Software Foundation; either version 2 of the License, or
    *    (at your option) any later version.
    *
    *    SDR-J is distributed in the hope that it will be useful,
    *    but WITHOUT ANY WARRANTY; without even the implied warranty of
    *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    *    GNU General Public License for more details.
    *
    *    You should have received a copy of the GNU General Public License
    *    along with SDR-J; if not, write to the Free Software
    *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
    *
    *	GUI design and implementation by Albrecht Lohoefener (c) 2016
    */
#include	<QSettings>
#include	"dab-constants.h"
#include	"gui.h"
#include	"CAudio.h"

#ifdef	HAVE_DABSTICK
#include	"dabstick.h"
#endif
#ifdef	HAVE_SDRPLAY
#include	"sdrplay.h"
#endif
#ifdef	HAVE_RTL_TCP
#include	"rtl_tcp_client.h"
#endif
#ifdef	HAVE_AIRSPY
#include	"airspy-handler.h"
#endif
#if HAVE_RAWFILE
#include	"rawfile.h"
#endif
/**
  *	We use the creation function merely to set up the
  *	user interface and make the connections between the
  *	gui elements and the handling agents. All real action
  *	is embedded in actions, initiated by gui buttons
  */
RadioInterface::RadioInterface(QSettings	*Si,
                               QString		device,
                               uint8_t		dabMode,
                               QString     dabBand,
                               QObject    *parent): QObject(parent)
{
    //	Before printing anything, we set
    setlocale(LC_ALL, "");

    running = false;
    scanMode = false;
    isSynced = UNSYNCED;
    isFICCRC = false;
    LastCurrentManualGain = 0;

    dabSettings = Si;
    input_device = device;
    this->dabBand = dabBand == "BAND III" ? BAND_III : L_BAND;

    // First we have to read the settings from the INI-file
    int16_t latency = dabSettings -> value("latency", 1). toInt(); // latency is used to allow different settings for different situations wrt the output buffering
    threshold = dabSettings -> value("threshold", 3). toInt(); // threshold is used in the phaseReference class as threshold for checking the validity of the correlation result
    autoStart = dabSettings -> value("autoStart", 0). toInt() != 0;

    // Read channels from the settings
    dabSettings -> beginGroup("channels");
    int channelcount = dabSettings ->value("channelcout", 0).toInt();
    for(int i = 1; i <= channelcount; i++)
    {
        QStringList SaveChannel = dabSettings -> value("channel/" + QString::number(i)).toStringList();
        stationList. append(SaveChannel. first(), SaveChannel. last());
    }
    dabSettings -> endGroup();
    stationList. sort();

    p_stationModel = QVariant::fromValue(stationList.getList());
    emit stationModelChanged();

    // Add image provider for the MOT slide show
    MOTImage = new MOTImageProvider;

    //	the name of the device is passed on from the main program
    if(!setDevice(input_device))
        emit showErrorMessage("Error while opening input device \"" + device + "\"");

    /**
    *	With this GUI there is no choice for the output channel,
    *	It is the soundcard, so just allocate it
    */
    AudioBuffer		= new RingBuffer<int16_t>(2 * 32768);
    Audio		= new CAudio(AudioBuffer, latency);

    setModeParameters(dabMode);

    /**
    *	The actual work is done elsewhere: in ofdmProcessor
    *	and ofdmDecoder for the ofdm related part, ficHandler
    *	for the FIC's and mscHandler for the MSC.
    *	The ficHandler shares information with the mscHandler
    *	but the handlers do not change each others modes.
    */
    my_mscHandler	= new mscHandler(this,
                                     &dabModeParameters,
                                     AudioBuffer,
                                     false);
    my_ficHandler	= new ficHandler(this);

    /**
    *	The default for the ofdmProcessor depends on
    *	the input device, note that in this setup the
    *	device is selected on start up and cannot be changed.
    */

    my_ofdmProcessor = new ofdmProcessor(inputDevice,
                                         &dabModeParameters,
                                         this,
                                         my_mscHandler,
                                         my_ficHandler,
                                         threshold,
                                         3);

    //	Set timer to check the FIC CRC
    connect(&CheckFICTimer, SIGNAL(timeout(void)), this, SLOT(CheckFICTimerTimeout(void)));
    connect(&StationTimer, SIGNAL(timeout(void)), this, SLOT(StationTimerTimeout(void)));
    CurrentFrameErrors = -1;
}

RadioInterface::~RadioInterface()
{
    fprintf(stderr, "deleting radioInterface\n");
}
/**
 * \brief returns the licenses for all the relative libraries plus application version information
 */
const QVariantMap RadioInterface::licenses(){
    QVariantMap ret;
    // Set application version
    QString InfoText;
    InfoText += "welle.io version: " + QString(CURRENT_VERSION) + "\n";
    InfoText += "Build on: " + QString(__TIMESTAMP__);
    ret.insert("version", InfoText);

    // Read graph license
    QFile File(":/QML/images/NOTICE.txt");
    File.open(QFile::ReadOnly);
    QByteArray FileContent = File.readAll();

    // Set graph license content
    ret.insert("graphLicense", FileContent);

    // Read license
    QFile File2(":/license");
    File2.open(QFile::ReadOnly);
    QByteArray FileContent2 = File2.readAll();

    // Set license content
    ret.insert("license", FileContent2);
    return ret;
}

/**
  *	\brief At the end, we might save some GUI values
  *	The QSettings could have been the class variable as well
  *	as the parameter
  */
void	RadioInterface::dumpControlState(QSettings *s)
{
    if(s == NULL)	// cannot happen
        return;

    //	Remove channels from previous invocation ...
    s -> beginGroup("channels");
    int ChannelCount = s -> value("channelcout").toInt();
    for(int i = 1; i <= ChannelCount; i++)
        s -> remove("channel/" + QString::number(i));

    //	... and save the current set
    ChannelCount = stationList. count();
    s -> setValue("channelcout",
                  QString::number(ChannelCount));

    for(int i = 1; i <= ChannelCount; i++)
        s -> setValue("channel/" + QString::number(i),
                      stationList. getStationAt(i - 1));
    dabSettings -> endGroup();
}
//
///	the values for the different Modes:
void	RadioInterface::setModeParameters(uint8_t Mode)
{

    if(Mode == 2)
    {
        dabModeParameters. dabMode	= 2;
        dabModeParameters. L		= 76;		// blocks per frame
        dabModeParameters. K		= 384;		// carriers
        dabModeParameters. T_null	= 664;		// null length
        dabModeParameters. T_F	= 49152;	// samples per frame
        dabModeParameters. T_s	= 638;		// block length
        dabModeParameters. T_u	= 512;		// useful part
        dabModeParameters. guardLength	= 126;
        dabModeParameters. carrierDiff	= 4000;
    }
    else if(Mode == 4)
    {
        dabModeParameters. dabMode		= 4;
        dabModeParameters. L			= 76;
        dabModeParameters. K			= 768;
        dabModeParameters. T_F		= 98304;
        dabModeParameters. T_null		= 1328;
        dabModeParameters. T_s		= 1276;
        dabModeParameters. T_u		= 1024;
        dabModeParameters. guardLength	= 252;
        dabModeParameters. carrierDiff	= 2000;
    }
    else if(Mode == 3)
    {
        dabModeParameters. dabMode		= 3;
        dabModeParameters. L			= 153;
        dabModeParameters. K			= 192;
        dabModeParameters. T_F		= 49152;
        dabModeParameters. T_null		= 345;
        dabModeParameters. T_s		= 319;
        dabModeParameters. T_u		= 256;
        dabModeParameters. guardLength	= 63;
        dabModeParameters. carrierDiff	= 2000;
    }
    else  	// default = Mode I
    {
        dabModeParameters. dabMode		= 1;
        dabModeParameters. L			= 76;
        dabModeParameters. K			= 1536;
        dabModeParameters. T_F		= 196608;
        dabModeParameters. T_null		= 2656;
        dabModeParameters. T_s		= 2552;
        dabModeParameters. T_u		= 2048;
        dabModeParameters. guardLength	= 504;
        dabModeParameters. carrierDiff	= 1000;
    }

    /*spectrumBuffer = new DSPCOMPLEX [dabModeParameters. T_u];
        memset (spectrumBuffer, 0,
                      dabModeParameters.T_u * sizeof (DSPCOMPLEX));*/
    spectrum_fft_handler = new common_fft(dabModeParameters. T_u);
}

struct dabFrequencies
{
    const char	*key;
    int	fKHz;
};

struct dabFrequencies bandIII_frequencies [] =
{
    {"5A",	174928},
    {"5B",	176640},
    {"5C",	178352},
    {"5D",	180064},
    {"6A",	181936},
    {"6B",	183648},
    {"6C",	185360},
    {"6D",	187072},
    {"7A",	188928},
    {"7B",	190640},
    {"7C",	192352},
    {"7D",	194064},
    {"8A",	195936},
    {"8B",	197648},
    {"8C",	199360},
    {"8D",	201072},
    {"9A",	202928},
    {"9B",	204640},
    {"9C",	206352},
    {"9D",	208064},
    {"10A",	209936},
    {"10B", 211648},
    {"10C", 213360},
    {"10D", 215072},
    {"11A", 216928},
    {"11B",	218640},
    {"11C",	220352},
    {"11D",	222064},
    {"12A",	223936},
    {"12B",	225648},
    {"12C",	227360},
    {"12D",	229072},
    {"13A",	230748},
    {"13B",	232496},
    {"13C",	234208},
    {"13D",	235776},
    {"13E",	237488},
    {"13F",	239200},
    {NULL, 0}
};

struct dabFrequencies Lband_frequencies [] =
{
    {"LA", 1452960},
    {"LB", 1454672},
    {"LC", 1456384},
    {"LD", 1458096},
    {"LE", 1459808},
    {"LF", 1461520},
    {"LG", 1463232},
    {"LH", 1464944},
    {"LI", 1466656},
    {"LJ", 1468368},
    {"LK", 1470080},
    {"LL", 1471792},
    {"LM", 1473504},
    {"LN", 1475216},
    {"LO", 1476928},
    {"LP", 1478640},
    {NULL, 0}
};

static
const char *table12 [] =
{
    "none",
    "news",
    "current affairs",
    "information",
    "sport",
    "education",
    "drama",
    "arts",
    "science",
    "talk",
    "pop music",
    "rock music",
    "easy listening",
    "light classical",
    "classical music",
    "other music",
    "wheather",
    "finance",
    "children\'s",
    "factual",
    "religion",
    "phone in",
    "travel",
    "leisure",
    "jazz and blues",
    "country music",
    "national music",
    "oldies music",
    "folk music",
    "entry 29 not used",
    "entry 30 not used",
    "entry 31 not used"
};

const char *RadioInterface::get_programm_type_string(uint8_t type)
{
    if(type > 0x40)
    {
        fprintf(stderr, "GUI: programmtype wrong (%d)\n", type);
        return (table12 [0]);
    }

    return table12 [type];
}

static
const char *table9 [] =
{
    "unknown",
    "Albanian",
    "Breton",
    "Catalan",
    "Croatian",
    "Welsh",
    "Czech",
    "Danish",
    "German",
    "English",
    "Spanish",
    "Esperanto",
    "Estonian",
    "Basque",
    "Faroese",
    "French",
    "Frisian",
    "Irish",
    "Gaelic",
    "Galician",
    "Icelandic",
    "Italian",
    "Lappish",
    "Latin",
    "Latvian",
    "Luxembourgian",
    "Lithuanian",
    "Hungarian",
    "Maltese",
    "Dutch",
    "Norwegian",
    "Occitan",
    "Polish",
    "Postuguese",
    "Romanian",
    "Romansh",
    "Serbian",
    "Slovak",
    "Slovene",
    "Finnish",
    "Swedish",
    "Tuskish",
    "Flemish",
    "Walloon"
};

const char *RadioInterface::get_programm_language_string(uint8_t language)
{
    if(language > 43)
    {
        fprintf(stderr, "GUI: wrong language (%d)\n", language);
        return table9 [0];
    }
    return table9 [language];
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
//
//	The public slots are called from other places within the dab software
//	so please provide some implementation, perhaps an empty one
//
//	a slot called by the ofdmprocessor
void	RadioInterface::set_fineCorrectorDisplay(int v)
{
    fineCorrector = v;
    emit displayFreqCorr(coarseCorrector + v);
}

//	a slot called by the ofdmprocessor
void	RadioInterface::set_coarseCorrectorDisplay(int v)
{
    coarseCorrector = v * kHz(1);
    emit displayFreqCorr(coarseCorrector + fineCorrector);
}
/**
  *	clearEnsemble
  *	on changing settings, we clear all things in the gui
  *	related to the ensemble.
  *	The function is called from "deep" within the handling code
  *	Potentially a dangerous approach, since the fic handler
  *	might run in a separate thread and generate data to be displayed
  */
void	RadioInterface::clearEnsemble(void)
{
    //
    //	it obviously means: stop processing
    my_mscHandler		-> stopProcessing();
    my_ficHandler		-> clearEnsemble();
    my_ofdmProcessor	-> coarseCorrectorOn();
    my_ofdmProcessor	-> reset();
}

//
//	a slot, called by the fic/fib handlers
void	RadioInterface::addtoEnsemble(const QString &s)
{
    //	Add new station into list
    if(!s.contains("data") && !stationList.contains(s))
    {
        stationList.append(s, currentChannel);

        //fprintf (stderr,"Found station %s\n", s.toStdString().c_str());
        emit foundChannelCount(stationList. count());
    }
}

//
///	a slot, called by the fib processor
void	RadioInterface::nameofEnsemble(int id, const QString &v)
{
    (void)id;
    (void)v;
    my_ofdmProcessor	-> coarseCorrectorOff();
}

void	RadioInterface::show_frameErrors(int s)
{
    CurrentFrameErrors = s;

    // Activate a timer to reset the frequency sychronisation if the FIC CRC is constant false
    if((CurrentFrameErrors != 0) && (!StationTimer.isActive()))
        StationTimer.start(10000); // 10 s

    emit displayFrameErrors(s);
}

void	RadioInterface::show_rsErrors (int s)
{
    emit displayRSErrors(s);
}

void	RadioInterface::show_aacErrors (int s)
{
    emit displayAACErrors(s);
}

///	called from the ofdmDecoder, which computes this for each frame
void	RadioInterface::show_snr(int s)
{
    emit signalPower(s);
}

///	just switch a color, obviously GUI dependent, but called
//	from the ofdmprocessor
void	RadioInterface::setSynced(char b)
{
    isSynced = b;
    switch(isSynced)
    {
    case SYNCED:
        emit syncFlag(true);
        break;

    default:
        emit syncFlag(false);
        break;
    }
}

//	showLabel is triggered by the message handler
//	the GUI may decide to ignore this
void	RadioInterface::showLabel(QString s)
{
    emit stationText("");
    //	The horizontal text alignment is not working if
    //	the text is not reset. Maybe this is a bug in QT
    emit stationText(s);
}
//
//	showMOT is triggered by the MOT handler,
void	RadioInterface::showMOT(QByteArray data, int subtype, QString s)
{
    (void)data;
    (void)subtype;
    (void)s;

    QPixmap p(320, 240);
    p.loadFromData(data, subtype == 0 ? "GIF" :
                   subtype == 1 ? "JPEG" :
                   subtype == 2 ? "BMP" : "PNG");

    MOTImage->setPixmap(p);
    emit motChanged();
}

//
//	sendDatagram is triggered by the ip handler, just ignore
void	RadioInterface::sendDatagram(char *data, int length)
{
    (void)data;
    (void)length;
}

/**
  *	\brief changeinConfiguration
  *	No idea yet what to do, so just give up
  *	with what we were doing. The user will -eventually -
  *	see the new configuration from which he can select
  */
void	RadioInterface::changeinConfiguration(void)
{
    if(running)
    {
        Audio		-> stop();
        inputDevice		-> stopReader();
        inputDevice		-> resetBuffer();
        running		= false;
    }
}
//
//	The audio is sent back from the audio decoder to the GUI
//	The gui will sent it to the appropriate soundhandler,
//	which for this GUI is the soundcard
//	Note the - when shutting down - some signals might
//	still wait for handling
void	RadioInterface::newAudio(int rate)
{
    if(running)
        Audio	-> audioOut(rate);
}

//	if so configured, the function might be triggered
//	from the message decoding software. The GUI
//	might decide to ignore the data sent
void	RadioInterface::show_mscErrors(int er)
{
    emit displayMSCErrors(er);
    fprintf(stderr, "displayMSCErrors: %i\n", er);
}
//
//	a slot, called by the iphandler
void	RadioInterface::show_ipErrors(int er)
{
    (void)er;
}
//
//	These are signals, not appearing in the other GUI's
void    RadioInterface::setStereo(bool isStereo)
{
    if(isStereo)
        emit audioType("Stereo");
    else
        emit audioType("Mono");
}
//
//
void	RadioInterface::startChannelScanClick(void)
{
    //
    //	if running: stop the input
    inputDevice	-> stopReader();
    //	Clear old channels
    stationList. reset();
    //	start the radio
    currentChannel = dabBand == BAND_III ?
                     bandIII_frequencies [0]. key :
                     Lband_frequencies [0]. key;
    set_channelSelect(currentChannel);
    emit currentStation(currentChannel);
    emit foundChannelCount(0);
    setStart();
    my_ofdmProcessor	-> reset();
    my_ofdmProcessor	-> set_scanMode(true, currentChannel);
    scanMode		= true;
}

void	RadioInterface::stopChannelScanClick(void)
{
    //	Stop channel scan
    my_ofdmProcessor	-> set_scanMode(false, currentChannel);
    ScanChannelTimer. stop();
    scanMode	= false;

    emit currentStation("No Station");

    //	Sort stations
    stationList. sort();
    p_stationModel = QVariant::fromValue(stationList.getList());
    emit stationModelChanged();
}

QString	RadioInterface::nextChannel(QString currentChannel)
{
    int16_t	i;
    struct dabFrequencies *finger;
    if(dabBand == BAND_III)
        finger = bandIII_frequencies;
    else
        finger = Lband_frequencies;

    for(i = 1; finger [i]. key != NULL; i ++)
        if(finger [i - 1]. key == currentChannel)
            return QString(finger [i]. key);
    return QString("");
}

//
//	The ofdm processor is "conditioned" to send one signal
//	per "scanning tour". This signal is either "false"
//	if we are pretty certain that the channel does not contain
//	a signal, or "true" if there is a fair chance that the
//	channel contains useful data
void    RadioInterface::setSignalPresent(bool isSignal)
{
    if(isSignal)  		// may be a channel, give it time
    {
        connect(&ScanChannelTimer, SIGNAL(timeout(void)),
                this, SLOT(end_of_waiting_for_stations(void)));
        ScanChannelTimer. start(10000);
        return;
    }
    currentChannel = nextChannel(currentChannel);
    if(currentChannel == QString(""))
    {
        emit channelScanStopped();
        emit currentStation("No Station");
        //	Sort stations
        stationList. sort();

        p_stationModel = QVariant::fromValue(stationList.getList());
        emit stationModelChanged();
        return;
    }
    set_channelSelect(currentChannel);
    emit currentStation(currentChannel);
    my_ofdmProcessor	-> reset();
    my_ofdmProcessor	-> set_scanMode(true, currentChannel);
}

void	RadioInterface::show_ficSuccess(bool b)
{
    isFICCRC = b;

    emit ficFlag(b);
}

void	RadioInterface::end_of_waiting_for_stations(void)
{
    disconnect(&ScanChannelTimer, SIGNAL(timeout(void)),
               this, SLOT(end_of_waiting_for_stations(void)));
    ScanChannelTimer. stop();
    currentChannel = nextChannel(currentChannel);
    if(currentChannel == QString(""))
    {
        emit channelScanStopped();
        emit currentStation("No Station");
        //	Sort stations
        stationList. sort();
        p_stationModel = QVariant::fromValue(stationList.getList());
        emit stationModelChanged();
        return;
    }
    set_channelSelect(currentChannel);
    emit currentStation(currentChannel);
    my_ofdmProcessor	-> reset();
    my_ofdmProcessor	-> set_scanMode(true, currentChannel);
}

void    RadioInterface::displayDateTime(int *DateTime)
{
    int Year	= DateTime [0];
    int Month	= DateTime [1];
    int Day		= DateTime [2];
    int Hour	= DateTime [3];
    int Minute	= DateTime [4];
    //int Seconds	= DateTime [5];
    int HourOffset	= DateTime [6];
    int MinuteOffset = DateTime[7];

    emit newDateTime(Year, Month, Day,
                     Hour + HourOffset, Minute + MinuteOffset);
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
//
//	Private slots relate to the modeling of the GUI
//
/**
  *	\brief setStart is a function that is called after pushing
  *	the start button.
  *	if "autoStart" == true, then the initializer will start
  *
  */
void	RadioInterface::setStart(void)
{
    bool	r = 0;
    if(running)		// only listen when not running yet
        return;
    //
    r = inputDevice		-> restartReader();
    qDebug("Starting %d\n", r);
    if(!r)
    {
        qDebug("Opening  input stream failed\n");
        return;
    }
    //
    //	Of course, starting the machine will generate a new instance
    //	of the ensemble, so the listing - if any - should be cleared
    clearEnsemble();		// the display
    //
    ///	this does not hurt
    Audio	-> restart();
    running = true;
}

/**
  *	\brief terminateProcess
  *	Pretty critical, since there are many threads involved
  *	A clean termination is what is needed, regardless of the GUI
  */
void	RadioInterface::terminateProcess(void)
{
    running			= false;
    inputDevice		-> stopReader();	// might be concurrent
    my_mscHandler		-> stopHandler();	// might be concurrent
    my_ofdmProcessor	-> stop();	// definitely concurrent
    Audio		-> stop();
    //
    //	everything should be halted by now
    dumpControlState(dabSettings);
    delete		my_ofdmProcessor;
    delete		my_ficHandler;
    delete		my_mscHandler;
    delete		Audio;
    Audio	= NULL;		// signals may be pending, so careful
    fprintf(stderr, "Termination started\n");
    delete		inputDevice;
    QApplication::quit();
}

//
/**
  *	\brief set_channelSelect
  *	Depending on the GUI the user might select a channel
  *	or some magic will cause a channel to be selected
  */
void	RadioInterface::set_channelSelect(QString s)
{
    int16_t	i;
    struct dabFrequencies *finger;
    bool	localRunning	= running;

    // Reset timeout to reset the tuner
    StationTimer.start(10000);
    CurrentFrameErrors = -1;

    if(localRunning)
    {
        clearEnsemble();
        Audio	-> stop();
        inputDevice		-> stopReader();
        inputDevice		-> resetBuffer();
    }

    tunedFrequency		= 0;
    if(dabBand == BAND_III)
        finger = bandIII_frequencies;
    else
        finger = Lband_frequencies;

    for(i = 0; finger [i]. key != NULL; i ++)
    {
        if(finger [i]. key == s)
        {
            tunedFrequency	= KHz(finger [i]. fKHz);
            break;
        }
    }

    if(tunedFrequency == 0)
        return;

    inputDevice		-> setVFOFrequency(tunedFrequency);

    if(localRunning)
    {
        Audio -> restart();
        inputDevice	 -> restartReader();
        my_ofdmProcessor	-> reset();
        running	 = true;
    }
    fprintf(stderr, "%s -> %d\n", s. toLatin1(). data(), tunedFrequency);
    emit displayCurrentChannel(s, tunedFrequency);
}

void	RadioInterface::updateTimeDisplay(void)
{
}

void	RadioInterface::autoCorrector_on(void)
{
    //	first the real stuff
    my_ficHandler		-> clearEnsemble();
    my_ofdmProcessor	-> coarseCorrectorOn();
    my_ofdmProcessor	-> reset();
}

/**
  *	\brief setDevice
  *	In this version, a device is specified in the command line
  *	or a default is taken. I.e., no dynamic switching of devices
  */
//
bool	RadioInterface::setDevice(QString s)
{
    bool	success;
#ifdef HAVE_AIRSPY
    if(s == "airspy")
    {
        inputDevice	= new airspyHandler(dabSettings, &success, true);
        if(!success)
        {
            delete inputDevice;
            inputDevice = new virtualInput();
            input_device = "no device";
            return false;
        }
        else
            return true;
    }
    else
#endif
#ifdef HAVE_RTL_TCP
        //	RTL_TCP might be working.
        if(s == "rtl_tcp")
        {
            inputDevice = new rtl_tcp_client(dabSettings, &success);
            if(!success)
            {
                delete inputDevice;
                inputDevice = new virtualInput();
                input_device = "no device";
                return false;
            }
            else
                return true;
        }
        else
#endif
#ifdef	HAVE_SDRPLAY
            if(s == "sdrplay")
            {
                inputDevice	= new sdrplay(dabSettings, &success, true);
                if(!success)
                {
                    delete inputDevice;
                    inputDevice = new virtualInput();
                    input_device = "no device";
                    return false;
                }
                else
                    return true;
            }
            else
#endif
#ifdef	HAVE_DABSTICK
                if(s == "dabstick")
                {
                    inputDevice	= new dabStick(dabSettings, &success, false);
                    if(!success)
                    {
                        delete inputDevice;
                        inputDevice = new virtualInput();
                        input_device = "no device";
                        return false;
                    }
                    else
                        return true;
                }
                else
#endif
#ifdef	HAVE_RAWFILE
                if(s == "rawfile")
                {
                    inputDevice	= new rawFile(dabSettings, &success);
                    if(!success)
                    {
                        delete inputDevice;
                        inputDevice = new virtualInput();
                        input_device = "no device";
                        return false;
                    }
                    else
                        return true;
                }
                else
#endif
                {
                    // s == "no device"
                    //	and as default option, we have a "no device"
                    fprintf(stderr, "Unknown input device \"%s\". \n", s.toStdString().c_str());
                    inputDevice	= new virtualInput();
                    input_device = "no device";
                    return false;
                }
    return true;
}

void    RadioInterface::CheckFICTimerTimeout(void)
{
    if(!isFICCRC)
        return;
    //
    //	for now: we handle only audio services
    if(my_ficHandler -> kindofService(CurrentStation) !=
            AUDIO_SERVICE)
        return;

    //	Tune to station
    audiodata d;
    CheckFICTimer. stop();   // stop timer
    emit currentStation(CurrentStation. simplified());
    my_ficHandler -> dataforAudioService(CurrentStation, &d);
    my_mscHandler -> set_audioChannel(&d);
    showLabel(QString(" "));
    stationType(get_programm_type_string(d.programType));
    languageType(get_programm_language_string(d.language));
    bitrate(d. bitRate);
    if(d.ASCTy == 077)
        emit dabType("DAB+");
    else
        emit dabType("DAB");
}

void    RadioInterface::StationTimerTimeout(void)
{
    StationTimer.stop();

    // Reset if frame success rate is below 50 %
    if(CurrentFrameErrors > 3 && !scanMode)
    {
        fprintf(stderr, "Resetting tuner ...\n");

        // Reset current channel to force channelClick to do a new turn
        // This is very ugly but its working
        QString tmpCurrentChannel = currentChannel;
        currentChannel = "";

        // Tune to channel
        channelClick(CurrentStation, tmpCurrentChannel);
    }
}

void	RadioInterface::channelClick(QString StationName,
                                     QString ChannelName)
{
    setStart();
    if(ChannelName != currentChannel)
    {
        set_channelSelect(ChannelName);
        currentChannel = ChannelName;
        emit syncFlag(false); // Clear flags
    }

    CurrentStation = StationName;

    //	Start the checking of the FIC CRC.
    //	If the FIC CRC is ok we can tune to the channel
    CheckFICTimer. start(1000);
    emit currentStation("Tuning ...");
    emit stationText("");

    // Clear MOT slide show
    QPixmap p(320, 240);
    p.fill(Qt::transparent);
    MOTImage->setPixmap(p);
    emit motChanged();

    // Clear flags
    emit displayFrameErrors(0);
    emit ficFlag(false);
}

void RadioInterface::saveSettings(void)
{
    dumpControlState(dabSettings);
}

void RadioInterface::inputEnableAGCChange(bool checked)
{
    if(inputDevice)
    {
        inputDevice->setAgc(checked);
        if(!checked)
            inputDevice->setGain(LastCurrentManualGain);
    }
}

void RadioInterface::inputGainChange(double gain)
{
    if(inputDevice)
    {
        LastCurrentManualGain = (int) gain;
        inputDevice->setGain(LastCurrentManualGain);
    }
}

// This function is called by the QML GUI
void RadioInterface::updateSpectrum(QAbstractSeries *series)
{
    int Samples = 0;

    if(series == NULL)
        return;

    QXYSeries *xySeries = static_cast<QXYSeries *>(series);

    //	Delete old data
    spectrum_data. clear();

    qreal tunedFrequency_MHz = tunedFrequency / 1e6;
    qreal sampleFrequency_MHz = 2048000 / 1e6;
    qreal dip_MHz = sampleFrequency_MHz / dabModeParameters. T_u;

    qreal x(0);
    qreal y(0);
    qreal y_max(0);

    // Get FFT buffer
    DSPCOMPLEX *spectrumBuffer = spectrum_fft_handler->getVector();

    // Get samples
    if(inputDevice)
        Samples = inputDevice->getSamplesFromShadowBuffer(spectrumBuffer, dabModeParameters.T_u);

    // Continue only if we got data
    if(Samples <= 0)
        return;

    // Do FFT to get the spectrum
    spectrum_fft_handler->do_FFT();

    //	Process samples one by one
    for(int i = 0; i < dabModeParameters. T_u; i++)
    {
        int half_Tu = dabModeParameters. T_u / 2;

        //	Shift FFT samples
        if(i < half_Tu)
            y = abs(spectrumBuffer [i + half_Tu]);
        else
            y = abs(spectrumBuffer[i - half_Tu]);

        //	Find maximum value to scale the plotter
        if(y > y_max)
            y_max = y;

        x = (i * dip_MHz) + (tunedFrequency_MHz - (sampleFrequency_MHz / 2));
        spectrum_data.append(QPointF(x, y));
    }

    //	Set maximum of y-axis
    y_max = round(y_max) + 1;
    if(y_max > 0.0001)
        emit setYAxisMax(y_max);

    // Set x-axis min and max
    emit setXAxisMinMax(tunedFrequency_MHz - (sampleFrequency_MHz / 2), tunedFrequency_MHz + (sampleFrequency_MHz / 2));

    //	Set new data
    xySeries->replace(spectrum_data);
}

void RadioInterface::setErrorMessage(QString ErrorMessage)
{
    // Print only if we tune into a channel
    if(currentChannel != QString(""))
        emit showErrorMessage(ErrorMessage);
}
