// sample-rate = 16000 固定で down-sampling は行わない

var Recorder = function() {
	window.AudioContext = window.AudioContext || window.webkitAudioContext || window.mozAudioContext || window.msAudioContext;
	navigator.getUserMedia = navigator.getUserMedia || navigator.webkitGetUserMedia || navigator.mozGetUserMedia || navigator.msGetUserMedia;
	navigator.mediaDevices = navigator.mediaDevices || ((navigator.getUserMedia) ? {
		getUserMedia: function(c) {
			return new Promise(
				function(y, n) {
					navigator.getUserMedia(c, y, n);
				}
			);
		}
	} : null);

	// public オブジェクト
	var recorder_ = {
		// public プロパティ
		sampleRate: 16000,
		sampleRateElement: undefined,
		maxRecordingTime: 60000,
		maxRecordingTimeElement: undefined,
		// public メソッド
		resume: resume_,
		pause: pause_,
		isActive: isActive_,
		// イベントハンドラ
		resumeStarted: undefined,
		resumeEnded: undefined,
		recorded: undefined,
		pauseStarted: undefined,
		pauseEnded: undefined 
	};

	var state_ = -1;
	var audioContext_;
	var audioProcessor_;
	var audioProcessor_onaudioprocess_recorded_;
	var audioStream_;
	var audioProvider_;
	var audioSamplesPerSec_;
	var pcmData_;
	var waveData_;
	var waveDataBytes_;
	var waveFile_;
	var reason_;
	var maxRecordingTimeTimerId_;

	function trim(val,valmin,valmax) {
		if ( val < valmin ) return valmin;
		if ( val > valmax ) return valmax;
		return val;
	}

	// 各種変数の初期化
	async function initialize_() {
		// 録音関係の各種変数の初期化
		audioContext_ = new AudioContext({sampleRate: recorder_.sampleRate});
		await audioContext_.audioWorklet.addModule(URL.createObjectURL(new Blob([
			"registerProcessor('audioWorkletProcessor', class extends AudioWorkletProcessor {",
			"  constructor() {",
			"    super()",
			"  }",
			"  process(inputs, outputs, parameters) {",
			"    if (inputs.length > 0 && inputs[0].length > 0) {",
			"      if (inputs[0].length === 2) {",
			"        for (var j = 0; j < inputs[0][0].length; j++) {",
			"          inputs[0][0][j] = (inputs[0][0][j] + inputs[0][1][j]) / 2",
			"        }",
			"      }",
			"      this.port.postMessage(inputs[0][0], [inputs[0][0].buffer])",
			"    }",
			"    return true",
			"  }",
			"})"
		], {type: 'application/javascript'})));
		audioProcessor_ = new AudioWorkletNode(audioContext_, 'audioWorkletProcessor');
		audioProcessor_.bufferSize = 128;
		audioProcessor_onaudioprocess_recorded_ = function(event) {
			if ( state_ === 0 ) return; // for AudioWorklet
			var audioData = event.data;
			var pcmDataOffset = 1;
			var pcmDataIndex = pcmDataOffset;
			for (var audioDataIndex = 0; audioDataIndex < audioData.length; audioDataIndex++) {
				var pcm = trim(audioData[audioDataIndex]*32768|0,-32768,32767); // 小数 (0.0～1.0) を 整数 (-32768～32767) に変換...
				pcmData_[pcmDataIndex++] = (pcm >> 8) & 0xFF;
				pcmData_[pcmDataIndex++] = (pcm     ) & 0xFF;
			}
			if (recorder_.recorded) recorder_.recorded(pcmData_.subarray(pcmDataOffset,pcmDataIndex));
			if (state_ === 3) {
				state_ = 4;
				audioStream_.stopTracks(); audioStream_ = undefined;
				audioProvider_.disconnect(); audioProvider_ = undefined;
				audioProcessor_.disconnect();
			}
		};
		pcmData_ = new Uint8Array(1 + 16 + audioProcessor_.bufferSize * 2);
		reason_ = {code: 0, message: ""};
		maxRecordingTimeTimerId_ = null;
	}

	// 録音の開始
	async function resume_() {
		if (state_ !== -1 && state_ !== 0) { return false; }
		if (recorder_.resumeStarted) recorder_.resumeStarted();
		if (!window.AudioContext) {
			if (recorder_.pauseEnded) recorder_.pauseEnded({code: 2, message: "Unsupported AudioContext class"}, waveFile_);
			return true;
		}
		if (!navigator.mediaDevices) {
			if (recorder_.pauseEnded) recorder_.pauseEnded({code: 2, message: "Unsupported MediaDevices class"}, waveFile_);
			return true;
		}
		if (recorder_.sampleRateElement) recorder_.sampleRate = recorder_.sampleRateElement.value - 0;
		if (recorder_.maxRecordingTimeElement) recorder_.maxRecordingTime = recorder_.maxRecordingTimeElement.value - 0;
		if (state_ === 0 && recorder_.sampleRate !== audioSamplesPerSec_) {
			audioStream_ = null;
			audioProvider_ = null;
			audioProcessor_ = null;
			audioContext_.close();
			audioContext_ = null;
			state_ = -1;
		}
		if (state_ === -1) {
			// 各種変数の初期化
			await initialize_();
			state_ = 0;
		}
		audioSamplesPerSec_ = audioContext_.sampleRate;
		if (audioSamplesPerSec_ === 0) {
			reason_.code = 2;
			reason_.message = "Unsupported sample rate: " + audioContext_.sampleRate + "Hz";
			if (recorder_.pauseEnded) recorder_.pauseEnded(reason_, waveFile_);
			return true;
		}
		state_ = 1;
		if (!recorder_.recorded) {
			waveData_ = [];
			waveDataBytes_ = 0;
			waveData_.push(new ArrayBuffer(44));
			waveDataBytes_ += 44;
		}
		waveFile_ = null;
		reason_.code = 0;
		reason_.message = "";
		audioProcessor_.port.onmessage = audioProcessor_onaudioprocess_recorded_;
		navigator.mediaDevices.getUserMedia(
			{audio: {echoCancellation: false}, video: false}
		).then(
			function(audioStream) {
				audioStream.stopTracks = function() {
					var tracks = audioStream.getTracks();
					for (var i = 0; i < tracks.length; i++) {
						tracks[i].stop();
					}
					state_ = 0;
					if (waveData_) {
						var waveData = new DataView(waveData_[0]);
						waveData.setUint8(0, 0x52); // 'R'
						waveData.setUint8(1, 0x49); // 'I'
						waveData.setUint8(2, 0x46); // 'F'
						waveData.setUint8(3, 0x46); // 'F'
						waveData.setUint32(4, waveDataBytes_ - 8, true);
						waveData.setUint8(8, 0x57); // 'W'
						waveData.setUint8(9, 0x41); // 'A'
						waveData.setUint8(10, 0x56); // 'V'
						waveData.setUint8(11, 0x45); // 'E'
						waveData.setUint8(12, 0x66); // 'f'
						waveData.setUint8(13, 0x6D); // 'm'
						waveData.setUint8(14, 0x74); // 't'
						waveData.setUint8(15, 0x20); // ' '
						waveData.setUint32(16, 16, true);
						waveData.setUint16(20, 1, true); // formatTag
						waveData.setUint16(22, 1, true); // channels
						waveData.setUint32(24, audioSamplesPerSec_, true); // samplesPerSec
						waveData.setUint32(28, audioSamplesPerSec_ * 2 * 1, true); // bytesPseSec
						waveData.setUint16(32, 2 * 1, true); // bytesPerSample
						waveData.setUint16(34, 16, true); // bitsPerSample
						waveData.setUint8(36, 0x64); // 'd'
						waveData.setUint8(37, 0x61); // 'a'
						waveData.setUint8(38, 0x74); // 't'
						waveData.setUint8(39, 0x61); // 'a'
						waveData.setUint32(40, waveDataBytes_ - 44, true);
						waveFile_ = new Blob(waveData_, {type: "audio/wav"});
						waveFile_.samplesPerSec = audioSamplesPerSec_;
						waveFile_.samples = (waveDataBytes_ - 44) / (2 * 1);
						waveData_ = null;
						waveDataBytes_ = 0;
					}
					if (recorder_.pauseEnded) recorder_.pauseEnded(reason_, waveFile_);
				};
				if (state_ === 3) {
					state_ = 4;
					audioStream.stopTracks();
					return;
				}
				state_ = 2;
				audioStream_ = audioStream;
				audioProvider_ = audioContext_.createMediaStreamSource(audioStream_);
				audioProvider_.connect(audioProcessor_);
				audioProcessor_.connect(audioContext_.destination);
				startMaxRecordingTimeTimer_();
				if (recorder_.resumeEnded) recorder_.resumeEnded("MSB" + (audioSamplesPerSec_ / 1000 | 0) + "K");
			}
		).catch(
			function(error) {
				state_ = 0;
				reason_.code = 2;
				reason_.message = error.message;
				if (recorder_.pauseEnded) recorder_.pauseEnded(reason_, waveFile_);
			}
		);
		return true;
	}

	// 録音の停止
	function pause_() {
		if (state_ !== 2) {
			return false;
		}
		state_ = 3;
		if (recorder_.pauseStarted) recorder_.pauseStarted();
		stopMaxRecordingTimeTimer_();
		return true;
	}

	// 録音中かどうかの取得
	function isActive_() { return (state_ === 2); }

	// 録音の停止を自動的に行うためのタイマの開始
	function startMaxRecordingTimeTimer_() {
		if (recorder_.maxRecordingTime <= 0) {
			return;
		}
		stopMaxRecordingTimeTimer_();
		maxRecordingTimeTimerId_ = setTimeout(fireMaxRecordingTimeTimer_, recorder_.maxRecordingTime);
	}

	// 録音の停止を自動的に行うためのタイマの停止
	function stopMaxRecordingTimeTimer_() {
		if (maxRecordingTimeTimerId_ !== null) {
			clearTimeout(maxRecordingTimeTimerId_);
			maxRecordingTimeTimerId_ = null;
		}
	}

	// 録音の停止を自動的に行うためのタイマの発火
	function fireMaxRecordingTimeTimer_() {
		reason_.code = 1;
		reason_.message = "Exceeded max recording time";
		pause_();
	}

	// public オブジェクトの返却
	return recorder_;
}();

function amivoice_parse(result) {
	var json = JSON.parse(result);
	json.duration = (json.results && json.results[0]) ? json.results[0].endtime : 0;
	json.confidence = (json.results && json.results[0]) ? json.results[0].confidence : -1.0;
	return json;
}

var Wrp = function() {
	var wrp_ = {
		serverURL: "",
		serverURLElement: undefined,
		grammarFileNames: "",
		grammarFileNamesElement: undefined,
		profileId: "",
		profileIdElement: undefined,
		profileWords: "",
		profileWordsElement: undefined,
		segmenterProperties: "",
		segmenterPropertiesElement: undefined,
		keepFillerToken: "",
		keepFillerTokenElement: undefined,
		resultUpdatedInterval: "",
		resultUpdatedIntervalElement: undefined,
		extension: "",
		extensionElement: undefined,
		authorization: "",
		authorizationElement: undefined,
		codec: "",
		codecElement: undefined,
		resultType: "",
		resultTypeElement: undefined,
		issuerURL: "",
		issuerURLElement: undefined,
		sid: null,
		sidElement: undefined,
		spw: null,
		spwElement: undefined,
		epi: null,
		epiElement: undefined,
		connect: connect_,
		disconnect: disconnect_,
		feedDataResume: feedDataResume_,
//		feedData: feedData_,
		feedDataPause: feedDataPause_,
		isConnected: isConnected_,
		isActive: isActive_,
		issue: issue_,
		connectStarted: undefined,
		connectEnded: undefined,
		disconnectStarted: undefined,
		disconnectEnded: undefined,
		feedDataResumeStarted: undefined,
		feedDataResumeEnded: undefined,
		feedDataPauseStarted: undefined,
		feedDataPauseEnded: undefined,
		utteranceStarted: undefined,
		utteranceEnded: undefined,
		resultCreated: undefined,
		resultUpdated: undefined,
		resultFinalized: undefined,
		eventNotified: undefined,
		issueStarted: undefined,
		issueEnded: undefined 
	};

	var state_ = 0;
	var socket_;
	var reason_;
	var checkIntervalTimeoutTimerId_ = null;
	var interlock_ = false;
	var recorder_ = window.Recorder || null;

	if ( recorder_ ) {
		// 録音の開始処理が完了した時に呼び出されます。
		recorder_.resumeEnded = function(codec) {
			wrp_.codec = codec;
			if ( wrp_.codecElement ) wrp_.codecElement.value = wrp_.codec;
			if ( state_ == 0 ) { connect_(); } 
			else if ( state_ === 3 ) { state_ = 4; feedDataResume__(); } 
			else if ( state_ === 13 ) { state_ = 17; recorder_.pause(); } 
			else if ( state_ === 23 ) { state_ = 27; recorder_.pause(); }
		};

		// 録音の開始処理が失敗した時または録音の停止処理が完了した時に呼び出されます。
		recorder_.pauseEnded = function(reason) {
			if ( state_ == 0 ) {
				if ( wrp_.feedDataResumeStarted ) wrp_.feedDataResumeStarted();
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason);
			}
			else if ( state_ === 3 ) {
				state_ = 2;
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason);
				if ( interlock_ ) { disconnect_(); }
			}
			else if ( state_ === 4 ) {
				state_ = 34;
				reason_ = reason;
			}
			else if ( state_ === 5 ) {
				state_ = 36;
				reason_ = reason;
				feedDataPause__();
			}
			else if ( state_ === 6 ) {
				state_ = 36;
				reason_ = reason;
			}
			else if ( state_ === 7 ) {
				state_ = 2;
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason);
				if ( interlock_ ) { disconnect_(); }
			}
			else if ( state_ === 13 || state_ === 17 ) {
				state_ = 0;
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 23 || state_ === 27 ) {
				state_ = 8;
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				socket_.close();
			}
		};

		// 音声データが録音された時に呼び出され、認識サービスに音声データを渡す。
		recorder_.recorded = function(data) {
			if ( state_ === 5 ) {
				if ( data.byteOffset >= 1 ) {
					data = new Uint8Array(data.buffer, data.byteOffset - 1, 1 + data.length);
					data[0] = 0x70; // 'p'
					socket_.send(data);
				}
				else {
					var newData = new Uint8Array(1 + data.length);
					newData[0] = 0x70; // 'p'
					newData.set(data, 1);
					socket_.send(newData);
				}
			}
		};
	}

	// WebSocket のオープン
	function connect_() {
		if ( state_ !== 0 ) { return false; }
		if ( wrp_.connectStarted ) wrp_.connectStarted();
		if ( wrp_.serverURLElement ) wrp_.serverURL = wrp_.serverURLElement.value;
		if ( !wrp_.serverURL ) {
			if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
			return true;
		}
		try {
			if ( wrp_.serverURL.startsWith("http://") ) {
				wrp_.serverURL = "ws://" + wrp_.serverURL.substring(7);
			}
			else if ( wrp_.serverURL.startsWith("https://") ) {
				wrp_.serverURL = "wss://" + wrp_.serverURL.substring(8);
			}
			socket_ = new WebSocket(wrp_.serverURL);
		}
		catch (e) {
			if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
			return true;
		}
		state_ = 1;
		socket_.onopen = function(event) {
			state_ = 2;
			if ( wrp_.connectEnded ) wrp_.connectEnded();
			if ( interlock_ ) { feedDataResume_(); }
		};
		socket_.onclose = function(event) {
			if ( state_ === 1 ) {
				state_ = 0;
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 2 ) {
				state_ = 0;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 3 ) {
				state_ = 13;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( !reason_ ) { reason_ = {code: 3, message: "Disconnected from WebSocket server"}; }
			}
			else if ( state_ === 4 || state_ === 5 || state_ === 6 ) {
				if ( state_ != 6 ) { if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted(); }
				state_ = 17;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( !reason_ ) { reason_ = {code: 3, message: "Disconnected from WebSocket server"}; }
				if ( recorder_ ) { recorder_.pause(); }
				else {
					state_ = 0;
					if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
					if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				}
			}
			else if ( state_ === 7 ) {
				state_ = 17;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( !reason_ ) { reason_ = {code: 3, message: "Disconnected from WebSocket server"}; }
			}
			else if ( state_ === 8 ) {
				state_ = 0;
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 23 ) {
				state_ = 13;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
			}
			else if ( state_ === 27 ) {
				state_ = 17;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
			}
			else if ( state_ === 34 || state_ === 36 ) {
				state_ = 0;
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
		};
		socket_.onmessage = function(event) {
			var tag = event.data[0];
			var body = event.data.substring(2);
			if ( tag === 's' ) { // 音声データ送信開始コマンド応答
				if (body) {
					if ( state_ === 2 ) {
						state_ = 8;
						if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
						socket_.close();
					}
					else if ( state_ === 3 ) {
						state_ = 23;
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 4 ) {
						state_ = 7;
						reason_ = {code: 3, message: body};
						if ( recorder_ ) { recorder_.pause(); }
						else {
							state_ = 2;
							if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
						}
					}
					else if ( state_ === 5 || state_ === 6 ) {
						if ( state_ != 6 ) {
							if ( wrp_.feedDataPauseStarted ) wrp_.feedDataPauseStarted();
						}
						state_ = 27;
						reason_ = {code: 3, message: body};
						if (recorder_) { recorder_.pause(); }
						else {
							state_ = 8;
							if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
							if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
							socket_.close();
						}
					}
					else if ( state_ === 7 ) {
						state_ = 27;
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 34 || state_ === 36 ) {
						state_ = 8;
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
				}
				else {
					if ( state_ === 4 ) {
						state_ = 5;
						if (wrp_.feedDataResumeEnded) wrp_.feedDataResumeEnded();
					}
					else if ( state_ === 34 ) {
						state_ = 36;
						feedDataPause__();
					}
				}
			}
			else if ( tag === 'p' ) { // 音声データ送信コマンド応答
				if (body) {
					if ( state_ === 2 ) {
						state_ = 8;
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
					else if ( state_ === 3 ) {
						state_ = 23;
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 4 || state_ === 5 || state_ === 6 ) {
						if ( state_ != 6 ) { if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted(); }
						state_ = 27;
						reason_ = {code: 3, message: body};
						if (recorder_) { recorder_.pause(); }
						else {
							state_ = 8;
							if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
							if (wrp_.disconnectStarted) wrp_.disconnectStarted();
							socket_.close();
						}
					}
					else if ( state_ === 7 ) {
						state_ = 27;
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 34 || state_ === 36 ) {
						state_ = 8;
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
				}
			}
			else if ( tag === 'e' ) { // 音声データ送信停止コマンド応答
				if (body) {
					if ( state_ === 2 ) {
						state_ = 8;
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
					else if ( state_ === 3 ) {
						state_ = 23;
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 4 || state_ === 5 || state_ === 6 ) {
						if ( state_ != 6 ) { if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted(); }
						state_ = 27;
						reason_ = {code: 3, message: body};
						if (recorder_) { recorder_.pause(); }
						else {
							state_ = 8;
							if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
							if (wrp_.disconnectStarted) wrp_.disconnectStarted();
							socket_.close();
						}
					}
					else if ( state_ === 7 ) {
						state_ = 27;
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 34 || state_ === 36 ) {
						state_ = 8;
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
				}
				else {
					if ( state_ === 6 ) {
						state_ = 7;
						if (recorder_) { recorder_.pause(); }
						else {
							state_ = 2;
							if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded({code: 0, message: ""});
						}
					}
					else if ( state_ === 36 ) {
						state_ = 2;
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (interlock_) { disconnect_(); }
					}
				}
			}
			// S : 発話区間開始検出通知
			// E : 発話区間終了検出通知
			// C : 認識処理開始通知
			// U : 認識処理中通知
			// A : 認識処理結果通知
			// R : 認識処理結果通知
			// Q : 
			// G : サーバ内でのアクション結果通知

			else if ( tag === 'C' ) { if (wrp_.resultCreated) wrp_.resultCreated(); }
			else if ( tag === 'U' ) { if (wrp_.resultUpdated) wrp_.resultUpdated(body); }
			else if ( tag === 'A' ) { if (wrp_.resultFinalized) wrp_.resultFinalized(body); }
			else if ( tag === 'R' ) { if (wrp_.resultFinalized) wrp_.resultFinalized("\x01\x01\x01\x01\x01" + body); }
			else if ( tag === 'Q' ) { if (wrp_.eventNotified) wrp_.eventNotified(tag, body); }
			else if ( tag === 'G' ) { if (wrp_.eventNotified) wrp_.eventNotified(tag, body); }
		};
		reason_ = null;
		return true;
	}

	function disconnect_() {
		if ( state_ === 5 ) {
			interlock_ = true;
			return feedDataPause_();
		}
		if ( state_ !== 2 ) { return false; }
		if (wrp_.disconnectStarted) wrp_.disconnectStarted();
		state_ = 8;
		socket_.close();
		return true;
	}

	// 音声データの供給の開始
	function feedDataResume_() {
		if ( state_ === 0 ) {
			interlock_ = true;
			// <!-- for Safari
			if ( recorder_ && !recorder_.isActive() ) {
				recorder_.resume();
				return true;
			}
			// -->
			return connect_();
		}
		if ( state_ !== 2 ) { return false; }
		if (wrp_.feedDataResumeStarted) wrp_.feedDataResumeStarted();
		state_ = 3;
		if ( recorder_ && !recorder_.isActive() ) {
			recorder_.resume();
			return true;
		}
		state_ = 4;
		feedDataResume__();
		return true;
	}
	function feedDataResume__() {
		if (wrp_.grammarFileNamesElement) wrp_.grammarFileNames = wrp_.grammarFileNamesElement.value;
		if (wrp_.profileIdElement) wrp_.profileId = wrp_.profileIdElement.value;
		if (wrp_.profileWordsElement) wrp_.profileWords = wrp_.profileWordsElement.value;
		if (wrp_.segmenterPropertiesElement) wrp_.segmenterProperties = wrp_.segmenterPropertiesElement.value;
		if (wrp_.keepFillerTokenElement) wrp_.keepFillerToken = wrp_.keepFillerTokenElement.value;
		if (wrp_.resultUpdatedIntervalElement) wrp_.resultUpdatedInterval = wrp_.resultUpdatedIntervalElement.value;
		if (wrp_.extensionElement) wrp_.extension = wrp_.extensionElement.value;
		if (wrp_.authorizationElement) wrp_.authorization = wrp_.authorizationElement.value;
		if (wrp_.codecElement) wrp_.codec = wrp_.codecElement.value;
		if (wrp_.resultTypeElement) wrp_.resultType = wrp_.resultTypeElement.value;
		var command = "s ";
		if (wrp_.codec) { command += wrp_.codec; } else { command += "MSB16K"; }
		if (wrp_.grammarFileNames) {
			command += " " + wrp_.grammarFileNames;
			if ( wrp_.grammarFileNames.indexOf('\x01') != -1 && !wrp_.grammarFileNames.endsWith("\x01") ) { command += '\x01'; }
		}
		else { command += " \x01"; }
		if (wrp_.profileId) { command += " profileId=" + wrp_.profileId; }
		if (wrp_.profileWords) { command += " profileWords=\"" + wrp_.profileWords.replace(/"/g, "\"\"") + "\""; }
		if (wrp_.segmenterProperties) { command += " segmenterProperties=\"" + wrp_.segmenterProperties.replace(/"/g, "\"\"") + "\""; }
		if (wrp_.keepFillerToken) { command += " keepFillerToken=" + wrp_.keepFillerToken; }
		if (wrp_.resultUpdatedInterval) { command += " resultUpdatedInterval=" + wrp_.resultUpdatedInterval; }
		if (wrp_.extension) { command += " extension=\"" + wrp_.extension.replace(/"/g, "\"\"") + "\""; }
		if (wrp_.authorization) { command += " authorization=" + wrp_.authorization; }
		if (wrp_.resultType) { command += " resultType=" + wrp_.resultType; }
		socket_.send(command);
		return true;
	}

	// 音声データの供給の停止
	function feedDataPause_() {
		if ( state_ !== 5 ) { return false; }
		if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted();
		state_ = 6;
		feedDataPause__();
		return true;
	}
	function feedDataPause__() {
		var command = "e";
		socket_.send(command);
		return true;
	}

	function isConnected_() { return (state_ === 2 || state_ === 3 || state_ === 4 || state_ === 5 || state_ === 6 || state_ === 7 || state_ === 23 || state_ === 27 || state_ === 34 || state_ === 36); }
	function isActive_() { return (state_ === 5); }

	// サービス認証キー文字列の発行
	function issue_() {
		if (wrp_.issuerURLElement) wrp_.issuerURL = wrp_.issuerURLElement.value;
		if (wrp_.sidElement) wrp_.sid = wrp_.sidElement.value;
		if (wrp_.spwElement) wrp_.spw = wrp_.spwElement.value;
		if (wrp_.epiElement) wrp_.epi = wrp_.epiElement.value;
		if (!wrp_.sid) {
			alert("サービス ID が設定されていません。");
			if (wrp_.sidElement) wrp_.sidElement.focus();
			return false;
		}
		for (var i=0;i<wrp_.sid.length;i++) {
			var c = wrp_.sid.charCodeAt(i);
			if (!(c >= 0x30 && c <= 0x39 || c >= 0x61 && c <= 0x7A || c >= 0x41 && c <= 0x5A || c === 0x2D || c === 0x5F)) {
				if (wrp_.sidElement) alert("サービス ID に許されていない文字が使用されています。");
				if (wrp_.sidElement) wrp_.sidElement.focus();
				return false;
			}
		}
		if (!wrp_.spw) {
			alert("サービスパスワードが設定されていません。");
			if (wrp_.spwElement) wrp_.spwElement.focus();
			return false;
		}
		for (var i=0;i<wrp_.spw.length;i++) {
			var c = wrp_.spw.charCodeAt(i);
			if ( c < 0x20 || c > 0x7E ) {
				if (wrp_.spwElement) alert("サービスパスワードに許されていない文字が使用されています。");
				if (wrp_.spwElement) wrp_.spwElement.focus();
				return false;
			}
		}
		for (var i=0;i<wrp_.epi.length;i++) {
			var c = wrp_.epi.charCodeAt(i);
			if ( c < 0x30 || c > 0x39 ) {
				if (wrp_.epiElement) alert("有効期限に許されていない文字が使用されています。");
				if (wrp_.epiElement) wrp_.epiElement.focus();
				return false;
			}
		}
		if (wrp_.issueStarted) wrp_.issueStarted();
		var searchParams = "sid=" + encodeURIComponent(wrp_.sid) + "&spw=" + encodeURIComponent(wrp_.spw);
		if (wrp_.epi) { searchParams += "&epi=" + encodeURIComponent(wrp_.epi); }
		var httpRequest = new XMLHttpRequest();
		httpRequest.addEventListener("load", function(e) {
			if (e.target.status === 200) {
				if (wrp_.serviceAuthorizationElement) { wrp_.serviceAuthorizationElement.value = e.target.response; }
				else if (wrp_.authorizationElement) { wrp_.authorizationElement.value = e.target.response; }
				else { wrp_.serviceAuthorization = e.target.response; }
				if (wrp_.issueEnded) wrp_.issueEnded(e.target.response);
			}
			else { if (wrp_.issueEnded) wrp_.issueEnded(""); }
		});
		httpRequest.addEventListener("error", function(e) { if (wrp_.issueEnded) wrp_.issueEnded(""); });
		httpRequest.addEventListener("abort", function(e) { if (wrp_.issueEnded) wrp_.issueEnded(""); });
		httpRequest.addEventListener("timeout", function(e) { if (wrp_.issueEnded) wrp_.issueEnded(""); });
		httpRequest.open("POST", wrp_.issuerURL, true);
		httpRequest.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
		httpRequest.send(searchParams);
		return true;
	}

	wrp_.serverURL = window.location.protocol + "//" + window.location.host + window.location.pathname;
	wrp_.serverURL = wrp_.serverURL.substring(0, wrp_.serverURL.lastIndexOf('/') + 1);
	if (wrp_.serverURL.endsWith("/tool/")) { wrp_.serverURL = wrp_.serverURL.substring(0, wrp_.serverURL.length - 5); }
	wrp_.serverURL += "/";
	wrp_.grammarFileNames = "-a-general";
	wrp_.issuerURL = window.location.protocol + "//" + window.location.host + window.location.pathname;
	wrp_.issuerURL = wrp_.issuerURL.substring(0, wrp_.issuerURL.lastIndexOf('/'));
	if ( wrp_.issuerURL.indexOf("/tool",wrp_.issuerURL.length - 5) !== -1 ) { wrp_.issuerURL = wrp_.issuerURL.substring(0, wrp_.issuerURL.length - 5); }
	wrp_.issuerURL += "/issue_service_authorization";
	return wrp_;
}();
