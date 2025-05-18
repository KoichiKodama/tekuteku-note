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
		version: "Recorder/1.0.08",
		sampleRate: 16000,
		sampleRateElement: undefined,
		maxRecordingTime: 60000,
		maxRecordingTimeElement: undefined,
		downSampling: false,
		downSamplingElement: undefined,
		adpcmPacking: false,
		adpcmPackinglement: undefined,
		// public メソッド
		resume: resume_,
		pause: pause_,
		isActive: isActive_,
		pack: pack_,
		// イベントハンドラ
		resumeStarted: undefined,
		resumeEnded: undefined,
		recorded: undefined,
		pauseStarted: undefined,
		pauseEnded: undefined,
		TRACE: undefined
	};

	var state_ = -1;
	var audioContext_;
	var audioProcessor_;
	var audioProcessor_onaudioprocess_;
	var audioProcessor_onaudioprocess_recorded_;
	var audioProcessor_onaudioprocess_downSampling_;
	var audioProcessor_onaudioprocess_downSampling_recorded_;
	var audioStream_;
	var audioProvider_;
	var audioSamplesPerSec_;
	var audioDecimatationFactor_;
	var temporaryAudioData_;
	var temporaryAudioDataSamples_;
	var coefData_;
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

	function eval_audio_params(sample_rate) {
		switch (sample_rate) {
			case 48000: return [16000,3];
			case 44100: return [22050,2];
			case 22050: return [22050,1];
			case 16000: return [16000,1];
			case  8000: return [ 8000,1];
		}
		return [0,0];
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
		audioProcessor_onaudioprocess_ = function(event) {
			if ( state_ === 0 ) return; // for AudioWorklet
			var audioData = event.data;
			var pcmData = new Uint8Array(audioData.length * 2);
			var pcmDataIndex = 0;
			for (var audioDataIndex = 0; audioDataIndex < audioData.length; audioDataIndex++) {
				var pcm = trim(audioData[audioDataIndex]*32768|0,-32768,32767); // 小数 (0.0～1.0) を 整数 (-32768～32767) に変換...
				pcmData[pcmDataIndex++] = (pcm     ) & 0xFF;
				pcmData[pcmDataIndex++] = (pcm >> 8) & 0xFF;
			}
			waveData_.push(pcmData.buffer);
			waveDataBytes_ += pcmData.buffer.byteLength;
			if (state_ === 3) {
				state_ = 4;
				audioStream_.stopTracks(); audioStream_ = undefined;
				audioProvider_.disconnect(); audioProvider_ = undefined;
				audioProcessor_.disconnect();
				if (recorder_.TRACE) recorder_.TRACE("INFO: stopped recording");
			}
		};
		audioProcessor_onaudioprocess_recorded_ = function(event) {
			if ( state_ === 0 ) return; // for AudioWorklet
			var audioData = event.data;
			var pcmDataOffset = (ima_state_ > 0) ? 1 + 16 : 1;
			var pcmDataIndex = pcmDataOffset;
			for (var audioDataIndex = 0; audioDataIndex < audioData.length; audioDataIndex++) {
				var pcm = trim(audioData[audioDataIndex]*32768|0,-32768,32767); // 小数 (0.0～1.0) を 整数 (-32768～32767) に変換...
				pcmData_[pcmDataIndex++] = (pcm >> 8) & 0xFF;
				pcmData_[pcmDataIndex++] = (pcm     ) & 0xFF;
			}
			if (recorder_.recorded) recorder_.recorded(pcmData_.subarray(pcmDataOffset, pcmDataIndex));
			if (state_ === 3) {
				state_ = 4;
				audioStream_.stopTracks(); audioStream_ = undefined;
				audioProvider_.disconnect(); audioProvider_ = undefined;
				audioProcessor_.disconnect();
				if (recorder_.TRACE) recorder_.TRACE("INFO: stopped recording");
			}
		};
		audioProcessor_onaudioprocess_downSampling_ = function(event) {
			if ( state_ === 0 ) return; // for Safari and AudioWorklet
			var audioData = event.data;
			var audioDataIndex = 0;
			while (temporaryAudioDataSamples_ < temporaryAudioData_.length) {
				temporaryAudioData_[temporaryAudioDataSamples_++] = audioData[audioDataIndex++];
			}
			while (temporaryAudioDataSamples_ == temporaryAudioData_.length) {
				var pcmData = new Uint8Array((audioData.length / audioDecimatationFactor_ | 0) * 2);
				var pcmDataIndex = 0;
				for (var temporaryAudioDataIndex = audioDecimatationFactor_ - 1; temporaryAudioDataIndex + 20 < temporaryAudioData_.length; temporaryAudioDataIndex += audioDecimatationFactor_) {
					var pcm_float = 0.0;
					for (var i = 0; i <= 20; i++) {
						pcm_float += temporaryAudioData_[temporaryAudioDataIndex + i] * coefData_[i];
					}
					var pcm = trim(pcm_float*32768|0,-32768,32767); // 小数 (0.0～1.0) を 整数 (-32768～32767) に変換...
					pcmData[pcmDataIndex++] = (pcm     ) & 0xFF;
					pcmData[pcmDataIndex++] = (pcm >> 8) & 0xFF;
				}
				waveData_.push(pcmData.buffer);
				waveDataBytes_ += pcmData.buffer.byteLength;
				temporaryAudioDataSamples_ = 0;
				var temporaryAudioDataIndex = temporaryAudioData_.length - 20;
				while (temporaryAudioDataIndex < temporaryAudioData_.length) {
					temporaryAudioData_[temporaryAudioDataSamples_++] = temporaryAudioData_[temporaryAudioDataIndex++];
				}
				while (audioDataIndex < audioData.length) {
					temporaryAudioData_[temporaryAudioDataSamples_++] = audioData[audioDataIndex++];
				}
			}
			if ( state_ === 3 ) {
				state_ = 4;
				audioStream_.stopTracks(); audioStream_ = undefined;
				audioProvider_.disconnect(); audioProvider_ = undefined;
				audioProcessor_.disconnect();
				if (recorder_.TRACE) recorder_.TRACE("INFO: stopped recording");
			}
		};
		audioProcessor_onaudioprocess_downSampling_recorded_ = function(event) {
			if ( state_ === 0 ) return; // for Safari and AudioWorklet
			var audioData = event.data;
			var audioDataIndex = 0;
			while (temporaryAudioDataSamples_ < temporaryAudioData_.length) {
				temporaryAudioData_[temporaryAudioDataSamples_++] = audioData[audioDataIndex++];
			}
			while (temporaryAudioDataSamples_ == temporaryAudioData_.length) {
				var pcmDataOffset = (ima_state_ > 0) ? 1 + 16 : 1;
				var pcmDataIndex = pcmDataOffset;
				for (var temporaryAudioDataIndex = audioDecimatationFactor_ - 1; temporaryAudioDataIndex + 20 < temporaryAudioData_.length; temporaryAudioDataIndex += audioDecimatationFactor_) {
					var pcm_float = 0.0;
					for (var i = 0; i <= 20; i++) {
						pcm_float += temporaryAudioData_[temporaryAudioDataIndex + i] * coefData_[i];
					}
					var pcm = trim(pcm_float*32768|0,-32768,32767); // 小数 (0.0～1.0) を 整数 (-32768～32767) に変換...
					pcmData_[pcmDataIndex++] = (pcm >> 8) & 0xFF;
					pcmData_[pcmDataIndex++] = (pcm     ) & 0xFF;
				}
				if (recorder_.recorded) recorder_.recorded(pcmData_.subarray(pcmDataOffset, pcmDataIndex));
				temporaryAudioDataSamples_ = 0;
				var temporaryAudioDataIndex = temporaryAudioData_.length - 20;
				while (temporaryAudioDataIndex < temporaryAudioData_.length) {
					temporaryAudioData_[temporaryAudioDataSamples_++] = temporaryAudioData_[temporaryAudioDataIndex++];
				}
				while (audioDataIndex < audioData.length) {
					temporaryAudioData_[temporaryAudioDataSamples_++] = audioData[audioDataIndex++];
				}
			}
			if ( state_ === 3 ) {
				state_ = 4;
				audioStream_.stopTracks(); audioStream_ = undefined;
				audioProvider_.disconnect(); audioProvider_ = undefined;
				audioProcessor_.disconnect();
				if (recorder_.TRACE) recorder_.TRACE("INFO: stopped recording");
			}
		};
		[audioSamplesPerSec_,audioDecimatationFactor_] = eval_audio_params(audioContext_.sampleRate);
		if (audioDecimatationFactor_ > 1) {
			temporaryAudioData_ = new Float32Array(20 + ((audioProcessor_.bufferSize / audioDecimatationFactor_ >> 1) << 1) * audioDecimatationFactor_);
			temporaryAudioDataSamples_ = 0;
			coefData_ = new Float32Array(10 + 1 + 10);
			if (audioDecimatationFactor_ == 3) {
				coefData_[ 0] = -1.9186907e-2;
				coefData_[ 1] =  1.2144312e-2;
				coefData_[ 2] =  3.8677038e-2;
				coefData_[ 3] =  3.1580867e-2;
				coefData_[ 4] = -1.2342449e-2;
				coefData_[ 5] = -6.0144741e-2;
				coefData_[ 6] = -6.1757100e-2;
				coefData_[ 7] =  1.2462522e-2;
				coefData_[ 8] =  1.4362448e-1;
				coefData_[ 9] =  2.6923548e-1;
				coefData_[10] =  3.2090380e-1;
				coefData_[11] =  2.6923548e-1;
				coefData_[12] =  1.4362448e-1;
				coefData_[13] =  1.2462522e-2;
				coefData_[14] = -6.1757100e-2;
				coefData_[15] = -6.0144741e-2;
				coefData_[16] = -1.2342449e-2;
				coefData_[17] =  3.1580867e-2;
				coefData_[18] =  3.8677038e-2;
				coefData_[19] =  1.2144312e-2;
				coefData_[20] = -1.9186907e-2;
			} else {
				coefData_[ 0] =  6.91278819431317970157e-6;
				coefData_[ 1] =  3.50501872599124908447e-2;
				coefData_[ 2] = -6.93948777552577666938e-6;
				coefData_[ 3] = -4.52254377305507659912e-2;
				coefData_[ 4] =  6.96016786605468951166e-6;
				coefData_[ 5] =  6.34850487112998962402e-2;
				coefData_[ 6] = -6.97495897838962264359e-6;
				coefData_[ 7] = -1.05997055768966674805e-1;
				coefData_[ 8] =  6.98394205755903385580e-6;
				coefData_[ 9] =  3.18274468183517456055e-1;
				coefData_[10] =  4.99993026256561279297e-1;
				coefData_[11] =  3.18274468183517456055e-1;
				coefData_[12] =  6.98394205755903385580e-6;
				coefData_[13] = -1.05997055768966674805e-1;
				coefData_[14] = -6.97495897838962264359e-6;
				coefData_[15] =  6.34850487112998962402e-2;
				coefData_[16] =  6.96016786605468951166e-6;
				coefData_[17] = -4.52254377305507659912e-2;
				coefData_[18] = -6.93948777552577666938e-6;
				coefData_[19] =  3.50501872599124908447e-2;
				coefData_[20] =  6.91278819431317970157e-6;
			}
		}
		pcmData_ = new Uint8Array(1 + 16 + audioProcessor_.bufferSize * 2);
		reason_ = {code: 0, message: ""};
		maxRecordingTimeTimerId_ = null;
	}

	// 録音の開始
	async function resume_() {
		if (state_ !== -1 && state_ !== 0) {
			if (recorder_.TRACE) recorder_.TRACE("ERROR: can't start recording (invalid state: " + state_ + ")");
			return false;
		}
		if (recorder_.resumeStarted) recorder_.resumeStarted();
		if (!window.AudioContext) {
			if (recorder_.TRACE) recorder_.TRACE("ERROR: can't start recording (Unsupported AudioContext class)");
			if (recorder_.pauseEnded) recorder_.pauseEnded({code: 2, message: "Unsupported AudioContext class"}, waveFile_);
			return true;
		}
		if (!navigator.mediaDevices) {
			if (recorder_.TRACE) recorder_.TRACE("ERROR: can't start recording (Unsupported MediaDevices class)");
			if (recorder_.pauseEnded) recorder_.pauseEnded({code: 2, message: "Unsupported MediaDevices class"}, waveFile_);
			return true;
		}
		if (recorder_.sampleRateElement) recorder_.sampleRate = recorder_.sampleRateElement.value - 0;
		if (recorder_.maxRecordingTimeElement) recorder_.maxRecordingTime = recorder_.maxRecordingTimeElement.value - 0;
		if (recorder_.downSamplingElement) recorder_.downSampling = recorder_.downSamplingElement.checked;
		if (recorder_.adpcmPackingElement) recorder_.adpcmPacking = recorder_.adpcmPackingElement.checked;
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
		if (recorder_.downSampling) { [audioSamplesPerSec_,audioDecimatationFactor_] = eval_audio_params(audioContext_.sampleRate); }
		else {
			audioSamplesPerSec_ = audioContext_.sampleRate;
			audioDecimatationFactor_ = 1;
		}
		if (audioSamplesPerSec_ === 0) {
			if (recorder_.TRACE) recorder_.TRACE("ERROR: can't start recording (Unsupported sample rate: " + audioContext_.sampleRate + "Hz)");
			reason_.code = 2;
			reason_.message = "Unsupported sample rate: " + audioContext_.sampleRate + "Hz";
			if (recorder_.pauseEnded) recorder_.pauseEnded(reason_, waveFile_);
			return true;
		}
		state_ = 1;
		if (audioDecimatationFactor_ > 1) {
			for (var i = 0; i <= 20; i++) {
				temporaryAudioData_[i] = 0.0;
			}
			temporaryAudioDataSamples_ = 20;
		}
		if (!recorder_.recorded) {
			waveData_ = [];
			waveDataBytes_ = 0;
			waveData_.push(new ArrayBuffer(44));
			waveDataBytes_ += 44;
		}
		waveFile_ = null;
		reason_.code = 0;
		reason_.message = "";
		if (audioDecimatationFactor_ > 1) {
			if (recorder_.recorded) {
				audioProcessor_.port.onmessage = audioProcessor_onaudioprocess_downSampling_recorded_;
			} else {
				audioProcessor_.port.onmessage = audioProcessor_onaudioprocess_downSampling_;
			}
		} else {
			if (recorder_.recorded) {
				audioProcessor_.port.onmessage = audioProcessor_onaudioprocess_recorded_;
			} else {
				audioProcessor_.port.onmessage = audioProcessor_onaudioprocess_;
			}
		}
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
					if (audioDecimatationFactor_ > 1) {
						if (recorder_.TRACE) recorder_.TRACE("INFO: cancelled recording: " + audioContext_.sampleRate + "Hz -> " + audioSamplesPerSec_ + "Hz (" + audioProcessor_.bufferSize + " samples/buffer)");
					} else {
						if (recorder_.TRACE) recorder_.TRACE("INFO: cancelled recording: " + audioSamplesPerSec_ + "Hz (" + audioProcessor_.bufferSize + " samples/buffer)");
					}
					return;
				}
				state_ = 2;
				audioStream_ = audioStream;
				audioProvider_ = audioContext_.createMediaStreamSource(audioStream_);
				audioProvider_.connect(audioProcessor_);
				audioProcessor_.connect(audioContext_.destination);
				if (audioDecimatationFactor_ > 1) {
					if (recorder_.TRACE) recorder_.TRACE("INFO: started recording: " + audioContext_.sampleRate + "Hz -> " + audioSamplesPerSec_ + "Hz (" + audioProcessor_.bufferSize + " samples/buffer)");
				} else {
					if (recorder_.TRACE) recorder_.TRACE("INFO: started recording: " + audioSamplesPerSec_ + "Hz (" + audioProcessor_.bufferSize + " samples/buffer)");
				}
				startMaxRecordingTimeTimer_();
				// <!-- for ADPCM packing
				ima_state_ = (recorder_.adpcmPacking) ? 1 : 0;
				ima_state_last_ = 0;
				ima_state_step_index_ = 0;
				// -->
				if (recorder_.resumeEnded) recorder_.resumeEnded(((ima_state_ > 0) ? "" : "MSB") + (audioSamplesPerSec_ / 1000 | 0) + "K");
			}
		).catch(
			function(error) {
				state_ = 0;
				if (recorder_.TRACE) recorder_.TRACE("ERROR: can't start recording (" + error.message + ")");
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
			if (recorder_.TRACE) recorder_.TRACE("ERROR: can't stop recording (invalid state: " + state_ + ")");
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
		if (recorder_.TRACE) recorder_.TRACE("INFO: started auto pause timeout timer: " + recorder_.maxRecordingTime);
	}

	// 録音の停止を自動的に行うためのタイマの停止
	function stopMaxRecordingTimeTimer_() {
		if (maxRecordingTimeTimerId_ !== null) {
			clearTimeout(maxRecordingTimeTimerId_);
			maxRecordingTimeTimerId_ = null;
			if (recorder_.TRACE) recorder_.TRACE("INFO: stopped auto pause timeout timer: " + recorder_.maxRecordingTime);
		}
	}

	// 録音の停止を自動的に行うためのタイマの発火
	function fireMaxRecordingTimeTimer_() {
		if (recorder_.TRACE) recorder_.TRACE("INFO: fired auto pause timeout timer: " + recorder_.maxRecordingTime);
		reason_.code = 1;
		reason_.message = "Exceeded max recording time";
		pause_();
	}

	// <!-- for ADPCM packing
	var ima_step_size_table_ = [
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
		19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
		50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
		130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
		337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
		876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
		2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
		5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
		15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
	];
	var ima_step_adjust_table_ = [
		-1, -1, -1, -1, 2, 4, 6, 8
	]
	var ima_state_;
	var ima_state_last_;
	var ima_state_step_index_;
	function linear2ima_(pcm) {
		var step_size = ima_step_size_table_[ima_state_step_index_];
		var diff = pcm - ima_state_last_;
		var ima = 0x00;
		if (diff < 0) {
			ima = 0x08;
			diff = -diff;
		}
		var vpdiff = 0;
		if (diff >= step_size) {
			ima |= 0x04;
			diff -= step_size;
			vpdiff += step_size;
		}
		step_size >>= 1;
		if (diff >= step_size) {
			ima |= 0x02;
			diff -= step_size;
			vpdiff += step_size;
		}
		step_size >>= 1;
		if (diff >= step_size) {
			ima |= 0x01;
			vpdiff += step_size;
		}
		step_size >>= 1;
		vpdiff += step_size;
		if ((ima & 0x08) != 0) {
			ima_state_last_ -= vpdiff;
		} else {
			ima_state_last_ += vpdiff;
		}
		ima_state_last_ = trim(ima_state_last_,-32768,32767);
		ima_state_step_index_ += ima_step_adjust_table_[ima & 0x07];
		ima_state_step_index_ = trim(ima_state_step_index_,0,88);
		return ima;
	}
	// -->

	// PCM 音声データの ADPCM 音声データへの変換
	function pack_(data) {
		if (ima_state_ > 0) {
			var oldData = new DataView(data.buffer, data.byteOffset, data.byteLength);
			var dataIndex = 0;
			if (ima_state_ === 1) {
				data = new Uint8Array(data.buffer, data.byteOffset - 16, 16 + data.length / 4);
				data[dataIndex++] = 0x23; // '#'
				data[dataIndex++] = 0x21; // '!'
				data[dataIndex++] = 0x41; // 'A'
				data[dataIndex++] = 0x44; // 'D'
				data[dataIndex++] = 0x50; // 'P'
				data[dataIndex++] = 0x0A; // '\n'
				data[dataIndex++] = audioSamplesPerSec_ & 0xFF;
				data[dataIndex++] = (audioSamplesPerSec_ >> 8) & 0xFF;
				data[dataIndex++] = 1;
				data[dataIndex++] = 2;
				data[dataIndex++] = 0;
				data[dataIndex++] = 0;
				data[dataIndex++] = 1;
				data[dataIndex++] = 2;
				data[dataIndex++] = 0;
				data[dataIndex++] = 0;
				ima_state_ = 2;
			} else {
				data = new Uint8Array(data.buffer, data.byteOffset - 16, data.length / 4);
			}
			for (var i = 0; i < oldData.byteLength; i += 4) {
				var pcm1 = oldData.getInt16(i    , false);
				var pcm2 = oldData.getInt16(i + 2, false);
				var ima1 = linear2ima_(pcm1);
				var ima2 = linear2ima_(pcm2);
				data[dataIndex++] = (ima1 << 4) | ima2;
			}
		}
		return data;
	}

	// public オブジェクトの返却
	return recorder_;
}();

var Result = function() {
	var result_ = {
		version: "Result/1.0.04",
		parse: parse_,
		parseJSON: parseJSON_,
		parseTEXT: parseTEXT_,
		parseRAW: parseRAW_
	};

	function parse_(result) {
		try {
			return parseJSON_(result);
		} catch (e) {
			if ( result.indexOf("\x01") == -1 ) return parseTEXT_(result);
			return parseRAW_(result);
		}
	}

	function parseJSON_(result) {
		var json = JSON.parse(result);
		json.duration = (json.results && json.results[0]) ? json.results[0].endtime : 0;
		json.confidence = (json.results && json.results[0]) ? json.results[0].confidence : -1.0;
		return json;
	}

	function parseTEXT_(result) {
		return {
			text: result,
			duration: 0,
			confidence: -1.0,
			code: "",
			message: ""
		};
	}

	function parseRAW_(result) {
		var local = {
			buffer: "",
			bufferEnding: 0
		};
		var fields = result.split("\x01");
		var fields0 = fields[0].split("|");
		var i, j;
		for (i = 0; i < fields0.length; i++) {
			var written = fields0[i];
			if ( (j = written.indexOf(" ")) != -1 ) { written = written.slice(0,j); }
			if ( (j = written.indexOf(":")) != -1 ) { written = written.slice(0,j); }
			if ( (j = written.indexOf("\x03")) != -1 ) { written = written.slice(0,j); }
			append_(local,written);
		}
		return {
			text: local.buffer,
			duration: (fields[2]) ? parseInt(fields[2].split("-")[1]) : 0,
			confidence: (fields[1]) ? parseFloat(fields[1]) : -1.0,
			code: "",
			message: ""
		};
	}

	function append_(local, item) {
		if (item.length == 0) return;
		if (item == "<->") return;
		var itemState = 0;
		for (var i = 0; i < item.length; i++) {
			var c = item.charCodeAt(i);
			if (itemState == 0) {
				if (c == 0x005F) { break; }
				else if (c == 0x4E00 || c == 0x4E8C || c == 0x4E09 || c == 0x56DB || c == 0x4E94 || c == 0x516D || c == 0x4E03 || c == 0x516B || c == 0x4E5D) { itemState = 1; } // '一'～'九' 
				else if (c == 0x5341) { itemState = 2; } // '十'
				else if (c == 0x767E) { itemState = 4; } // '百'
				else if (c == 0x5343) { itemState = 8; } // '千'
				else { break; }
			}
			else {
				if (c == 0x005F) { item = item.substr(0, i) + item.substr(i + 1); break; }
				else if (c == 0x4E00 || c == 0x4E8C || c == 0x4E09 || c == 0x56DB || c == 0x4E94 || c == 0x516D || c == 0x4E03 || c == 0x516B || c == 0x4E5D) { if ((itemState & 1) != 0) { break; } else { itemState |= 1; } } // '一'～'九'
				else if (c == 0x5341) { if ((itemState & 2) != 0) { break; } else { itemState |= 2; itemState &= ~1; } } // '十'
				else if (c == 0x767E) { if ((itemState & 6) != 0) { break; } else { itemState |= 4; itemState &= ~1; } } // '百'
				else if (c == 0x5343) { if ((itemState & 14) != 0) { break; } else { itemState |= 8; itemState &= ~1; } } // '千'
				else { break; }
			}
		}
		item = item.replace(/_/g, " ");
		var itemBeginningChar = item.charCodeAt(0);
		var itemEndingChar = (item.length > 1) ? item.charCodeAt(item.length - 1) : 0;
		if (local.bufferEnding == 0) {
			var itemBeginning;
			var c = itemBeginningChar;
			if (c == 0x0020) { itemBeginning = 0; }
			else if (c == 0x0021 || c == 0x002C || c == 0x002E || c == 0x003A || c == 0x003B || c == 0x003F) { itemBeginning = 5; }
			else if (c == 0x3001 || c == 0x3002 || c == 0xFF01 || c == 0xFF0C || c == 0xFF0E || c == 0xFF1A || c == 0xFF1B || c == 0xFF1F) { itemBeginning = 6; }
			else { itemBeginning = 7; }
			if (itemBeginning == 0 || itemBeginning == 5 || itemBeginning == 6) {
				if (local.buffer.length > 0) { local.buffer = local.buffer.substr(0, local.buffer.length - 1); }
			}
		}
		else {
			var itemBeginning;
			var c = itemBeginningChar;
			if (c == 0x0020) {
				itemBeginning = 0;
			} else
			if ( c >= 0x0041 && c <= 0x005A || c >= 0x0061 && c <= 0x007A || c >= 0x0100 && c <= 0x0DFF || c >= 0x0E60 && c <= 0x01FF) { itemBeginning = 1; }
			else if (c >= 0xFF21 && c <= 0xFF3A || c >= 0xFF41 && c <= 0xFF5A) { itemBeginning = 2; }
			else if (c >= 0x0030 && c <= 0x0039) { itemBeginning = (local.bufferEnding == 8 && itemEndingChar == 0) ? 8 : 3; }
			else if (c >= 0xFF10 && c <= 0xFF19) { itemBeginning = (local.bufferEnding == 9 && itemEndingChar == 0) ? 9 : 4; }
			else if (c == 0x0021 || c == 0x002C || c == 0x002E || c == 0x003A || c == 0x003B || c == 0x003F) { itemBeginning = 5; }
			else if (c == 0x3001 || c == 0x3002 || c == 0xFF01 || c == 0xFF0C || c == 0xFF0E || c == 0xFF1A || c == 0xFF1B || c == 0xFF1F) { itemBeginning = 6; }
			else { itemBeginning = 7; }
			if (itemBeginning == 1 || 
				local.bufferEnding == 1 && (itemBeginning == 2 || itemBeginning == 3 || itemBeginning == 4 || itemBeginning == 7) || 
				local.bufferEnding == 2 && (itemBeginning == 2) || 
				local.bufferEnding == 3 && (itemBeginning == 3 || itemBeginning == 4) || 
				local.bufferEnding == 4 && (itemBeginning == 3 || itemBeginning == 4) || 
				local.bufferEnding == 5 && (itemBeginning == 2 || itemBeginning == 3 || itemBeginning == 4 || itemBeginning == 7) || 
				local.bufferEnding == 8 && (itemBeginning == 3 || itemBeginning == 4) || 
				local.bufferEnding == 9 && (itemBeginning == 3 || itemBeginning == 4)) { local.buffer += " "; }
		}
		local.buffer += item;
		c = (itemEndingChar == 0) ? itemBeginningChar : itemEndingChar;
		if (c == 0x0020) { local.bufferEnding = 0; }
		else if (c >= 0x0041 && c <= 0x005A || c >= 0x0061 && c <= 0x007A || c >= 0x0100 && c <= 0x0DFF || c >= 0x0E60 && c <= 0x01FF) { local.bufferEnding = 1; }
		else if (c >= 0xFF21 && c <= 0xFF3A || c >= 0xFF41 && c <= 0xFF5A) { local.bufferEnding = 2; }
		else if (c >= 0x0030 && c <= 0x0039) { local.bufferEnding = (itemEndingChar == 0) ? 8 : 3; }
		else if (c >= 0xFF10 && c <= 0xFF19) { local.bufferEnding = (itemEndingChar == 0) ? 9 : 4; }
		else if (c == 0x0021 || c == 0x002C || c == 0x002E || c == 0x003A || c == 0x003B || c == 0x003F) { local.bufferEnding = 5;}
		else if (c == 0x3001 || c == 0x3002 || c == 0xFF01 || c == 0xFF0C || c == 0xFF0E || c == 0xFF1A || c == 0xFF1B || c == 0xFF1F) { local.bufferEnding = 6; }
		else { local.bufferEnding = 7; }
	}

	return result_;
}();

var Wrp = function() {
	var wrp_ = {
		version: "Wrp/1.0.07",
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
		checkIntervalTime: 0,
		checkIntervalTimeElement: undefined,
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
		feedData: feedData_,
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
		issueEnded: undefined,
		TRACE: undefined
	};

	var state_ = 0;
	var socket_;
	var reason_;
	var checkIntervalTimeoutTimerId_ = null;
	var interlock_ = false;
	var recorder_ = window.Recorder || null;

	if ( recorder_ ) {
		recorder_.downSampling = true;
		recorder_.adpcmPacking = false;

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

		// 音声データが録音された時に呼び出されます。
		recorder_.recorded = function(data) {
			if ( state_ === 5 ) {
				data = recorder_.pack(data);
				feedData__(data);
			}
		};
	}

	// WebSocket のオープン
	function connect_() {
		if ( state_ !== 0 ) {
			if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't connect to WebSocket server (Invalid state: " + state_ + ")");
			return false;
		}
		if ( wrp_.connectStarted ) wrp_.connectStarted();
		if ( wrp_.serverURLElement ) wrp_.serverURL = wrp_.serverURLElement.value;
		if ( !wrp_.serverURL ) {
			if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't connect to WebSocket server (Missing server URL)");
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
			if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't connect to WebSocket server (" + e.message + ")");
			if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
			return true;
		}
		state_ = 1;
		socket_.onopen = function(event) {
			state_ = 2;
			if ( wrp_.TRACE ) wrp_.TRACE("INFO: connected to WebSocket server: " + wrp_.serverURL);
			if ( wrp_.connectEnded ) wrp_.connectEnded();
			if ( interlock_ ) { feedDataResume_(); }
		};
		socket_.onclose = function(event) {
			if ( state_ === 1 ) {
				state_ = 0;
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't connect to WebSocket server: " + wrp_.serverURL);
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 2 ) {
				state_ = 0;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 3 ) {
				state_ = 13;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
				if ( !reason_ ) { reason_ = {code: 3, message: "Disconnected from WebSocket server"}; }
			}
			else if ( state_ === 4 || state_ === 5 || state_ === 6 ) {
				if ( state_ != 6 ) { if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted(); }
				state_ = 17;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
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
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
				if ( !reason_ ) { reason_ = {code: 3, message: "Disconnected from WebSocket server"}; }
			}
			else if ( state_ === 8 ) {
				state_ = 0;
				if ( wrp_.TRACE ) wrp_.TRACE("INFO: disconnected from WebSocket server");
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
			else if ( state_ === 23 ) {
				state_ = 13;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
			}
			else if ( state_ === 27 ) {
				state_ = 17;
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
			}
			else if ( state_ === 34 || state_ === 36 ) {
				state_ = 0;
				if ( wrp_.feedDataPauseEnded ) wrp_.feedDataPauseEnded(reason_);
				if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
				if ( wrp_.TRACE ) wrp_.TRACE("ERROR: disconnected from WebSocket server");
				if ( wrp_.disconnectEnded ) wrp_.disconnectEnded();
				interlock_ = false;
			}
		};
		socket_.onmessage = function(event) {
			if ( wrp_.TRACE ) wrp_.TRACE("-> " + event.data);
			var tag = event.data[0];
			var body = event.data.substring(2);
			if ( tag === 's' ) {
				if (body) {
					if ( state_ === 2 ) {
						state_ = 8;
						stopCheckIntervalTimeoutTimer_();
						if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (" + body + ")");
						if ( wrp_.disconnectStarted ) wrp_.disconnectStarted();
						socket_.close();
					}
					else if ( state_ === 3 ) {
						state_ = 23;
						stopCheckIntervalTimeoutTimer_();
						if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (" + body + ")");
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 4 ) {
						state_ = 7;
						stopCheckIntervalTimeoutTimer_();
						if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (" + body + ")");
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
						stopCheckIntervalTimeoutTimer_();
						if ( wrp_.TRACE ) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (" + body + ")");
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
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (" + body + ")");
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 34 || state_ === 36 ) {
						state_ = 8;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (" + body + ")");
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
				}
				else {
					if ( state_ === 4 ) {
						state_ = 5;
						if (wrp_.TRACE) wrp_.TRACE("INFO: started feeding data to WebSocket server");
						startCheckIntervalTimeoutTimer_();
						if (wrp_.feedDataResumeEnded) wrp_.feedDataResumeEnded();
					}
					else if ( state_ === 34 ) {
						state_ = 36;
						if (wrp_.TRACE) wrp_.TRACE("INFO: started feeding data to WebSocket server");
						feedDataPause__();
					}
				}
			}
			else if ( tag === 'p' ) {
				if (body) {
					if ( state_ === 2 ) {
						state_ = 8;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't feed data to WebSocket server (" + body + ")");
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
					else if ( state_ === 3 ) {
						state_ = 23;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't feed data to WebSocket server (" + body + ")");
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 4 || state_ === 5 || state_ === 6 ) {
						if ( state_ != 6 ) { if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted(); }
						state_ = 27;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't feed data to WebSocket server (" + body + ")");
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
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't feed data to WebSocket server (" + body + ")");
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 34 || state_ === 36 ) {
						state_ = 8;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't feed data to WebSocket server (" + body + ")");
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
				}
			}
			else if ( tag === 'e' ) {
				if (body) {
					if ( state_ === 2 ) {
						state_ = 8;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't stop feeding data to WebSocket server (" + body + ")");
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
					else if ( state_ === 3 ) {
						state_ = 23;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't stop feeding data to WebSocket server (" + body + ")");
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 4 || state_ === 5 || state_ === 6 ) {
						if ( state_ != 6 ) { if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted(); }
						state_ = 27;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't stop feeding data to WebSocket server (" + body + ")");
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
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't stop feeding data to WebSocket server (" + body + ")");
						reason_ = {code: 3, message: body};
					}
					else if ( state_ === 34 || state_ === 36 ) {
						state_ = 8;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("ERROR: can't stop feeding data to WebSocket server (" + body + ")");
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (wrp_.disconnectStarted) wrp_.disconnectStarted();
						socket_.close();
					}
				}
				else {
					if ( state_ === 6 ) {
						state_ = 7;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("INFO: stopped feeding data to WebSocket server");
						if (recorder_) { recorder_.pause(); }
						else {
							state_ = 2;
							if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded({code: 0, message: ""});
						}
					}
					else if ( state_ === 36 ) {
						state_ = 2;
						stopCheckIntervalTimeoutTimer_();
						if (wrp_.TRACE) wrp_.TRACE("INFO: stopped feeding data to WebSocket server");
						if (wrp_.feedDataPauseEnded) wrp_.feedDataPauseEnded(reason_);
						if (interlock_) { disconnect_(); }
					}
				}
			}
			else if ( tag === 'S' ) {
				if (wrp_.utteranceStarted) wrp_.utteranceStarted(body);
				stopCheckIntervalTimeoutTimer_();
			}
			else if ( tag === 'E' ) { if (wrp_.utteranceEnded) wrp_.utteranceEnded(body); }
			else if ( tag === 'C' ) { if (wrp_.resultCreated) wrp_.resultCreated(); }
			else if ( tag === 'U' ) { if (wrp_.resultUpdated) wrp_.resultUpdated(body); }
			else if ( tag === 'A' ) { if (wrp_.resultFinalized) wrp_.resultFinalized(body); startCheckIntervalTimeoutTimer_(); }
			else if ( tag === 'R' ) { if (wrp_.resultFinalized) wrp_.resultFinalized("\x01\x01\x01\x01\x01" + body); startCheckIntervalTimeoutTimer_(); }
			else if ( tag === 'Q' ) { if (wrp_.eventNotified) wrp_.eventNotified(tag, body); }
			else if ( tag === 'G' ) { if (wrp_.eventNotified) wrp_.eventNotified(tag, body); }
		};
		reason_ = null;
		return true;
	}

	function disconnect_() {
		if ( state_ === 5 ) {
			interlock_ = true;
			if (recorder_) { recorder_.TRACE = wrp_.TRACE; }
			return feedDataPause_();
		}
		if ( state_ !== 2 ) {
			if (wrp_.TRACE) wrp_.TRACE("ERROR: can't disconnect from WebSocket server (Invalid state: " + state_ + ")");
			return false;
		}
		if (wrp_.disconnectStarted) wrp_.disconnectStarted();
		state_ = 8;
		socket_.close();
		return true;
	}

	// 音声データの供給の開始
	function feedDataResume_() {
		if ( state_ === 0 ) {
			interlock_ = true;
			if (recorder_) { recorder_.TRACE = wrp_.TRACE; }
			// <!-- for Safari
			if ( recorder_ && !recorder_.isActive() ) {
				recorder_.resume();
				return true;
			}
			// -->
			return connect_();
		}
		if ( state_ !== 2 ) {
			if (wrp_.TRACE) wrp_.TRACE("ERROR: can't start feeding data to WebSocket server (Invalid state: " + state_ + ")");
			return false;
		}
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
		if (wrp_.checkIntervalTimeElement) wrp_.checkIntervalTime = wrp_.checkIntervalTimeElement.value - 0;
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
		if (wrp_.TRACE) wrp_.TRACE("<- " + command);
		return true;
	}

	// 音声データの供給
	function feedData_(data) {
		if ( state_ !== 5 ) {
			if (wrp_.TRACE) wrp_.TRACE("ERROR: can't feed data to WebSocket server (Invalid state: " + state_ + ")");
			return false;
		}
		feedData__(data);
		return true;
	}
	function feedData__(data) {
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

	// 音声データの供給の停止
	function feedDataPause_() {
		if ( state_ !== 5 ) {
			if (wrp_.TRACE) wrp_.TRACE("ERROR: can't stop feeding data to WebSocket server (Invalid state: " + state_ + ")");
			return false;
		}
		if (wrp_.feedDataPauseStarted) wrp_.feedDataPauseStarted();
		state_ = 6;
		stopCheckIntervalTimeoutTimer_();
		feedDataPause__();
		return true;
	}
	function feedDataPause__() {
		var command = "e";
		socket_.send(command);
		if (wrp_.TRACE) wrp_.TRACE("<- " + command);
		return true;
	}

	function isConnected_() { return (state_ === 2 || state_ === 3 || state_ === 4 || state_ === 5 || state_ === 6 || state_ === 7 || state_ === 23 || state_ === 27 || state_ === 34 || state_ === 36); }
	function isActive_() { return (state_ === 5); }

	// 録音の停止を自動的に行うためのタイマの開始
	function startCheckIntervalTimeoutTimer_() {
		if ( wrp_.checkIntervalTime - 1000 <= 0 ) { return; }
		stopCheckIntervalTimeoutTimer_();
		checkIntervalTimeoutTimerId_ = setTimeout(fireCheckIntervalTimeoutTimer_, wrp_.checkIntervalTime - 1000);
		if (wrp_.TRACE) wrp_.TRACE("INFO: started check interval time timer: " + wrp_.checkIntervalTime + "(-1000)");
	}

	// 録音の停止を自動的に行うためのタイマの停止
	function stopCheckIntervalTimeoutTimer_() {
		if ( checkIntervalTimeoutTimerId_ !== null ) {
			clearTimeout(checkIntervalTimeoutTimerId_);
			checkIntervalTimeoutTimerId_ = null;
			if (wrp_.TRACE) wrp_.TRACE("INFO: stopped check interval time timer: " + wrp_.checkIntervalTime + "(-1000)");
		}
	}

	// 録音の停止を自動的に行うためのタイマの発火
	function fireCheckIntervalTimeoutTimer_() {
		if (wrp_.TRACE) wrp_.TRACE("INFO: fired check interval time timer: " + wrp_.checkIntervalTime + "(-1000)");
		feedDataPause_();
	}

	// サービス認証キー文字列の発行
	function issue_() {
		if (wrp_.issuerURLElement) wrp_.issuerURL = wrp_.issuerURLElement.value;
		if (wrp_.sidElement) wrp_.sid = wrp_.sidElement.value;
		if (wrp_.spwElement) wrp_.spw = wrp_.spwElement.value;
		if (wrp_.epiElement) wrp_.epi = wrp_.epiElement.value;
		if (!wrp_.sid) {
			if (wrp_.TRACE) wrp_.TRACE("ERROR: can't issue service authorization (Missing service id)");
			alert("サービス ID が設定されていません。");
			if (wrp_.sidElement) wrp_.sidElement.focus();
			return false;
		}
		for (var i=0;i<wrp_.sid.length;i++) {
			var c = wrp_.sid.charCodeAt(i);
			if (!(c >= 0x30 && c <= 0x39 || c >= 0x61 && c <= 0x7A || c >= 0x41 && c <= 0x5A || c === 0x2D || c === 0x5F)) {
				if (wrp_.TRACE) wrp_.TRACE("ERROR: can't issue service authorization (Illegal char in service id)");
				if (wrp_.sidElement) alert("サービス ID に許されていない文字が使用されています。");
				if (wrp_.sidElement) wrp_.sidElement.focus();
				return false;
			}
		}
		if (!wrp_.spw) {
			if (wrp_.TRACE) wrp_.TRACE("ERROR: can't issue service authorization (Missing service password)");
			alert("サービスパスワードが設定されていません。");
			if (wrp_.spwElement) wrp_.spwElement.focus();
			return false;
		}
		for (var i=0;i<wrp_.spw.length;i++) {
			var c = wrp_.spw.charCodeAt(i);
			if ( c < 0x20 || c > 0x7E ) {
				if (wrp_.TRACE) wrp_.TRACE("ERROR: can't issue service authorization (Illegal char in service password)");
				if (wrp_.spwElement) alert("サービスパスワードに許されていない文字が使用されています。");
				if (wrp_.spwElement) wrp_.spwElement.focus();
				return false;
			}
		}
		for (var i=0;i<wrp_.epi.length;i++) {
			var c = wrp_.epi.charCodeAt(i);
			if ( c < 0x30 || c > 0x39 ) {
				if (wrp_.TRACE) wrp_.TRACE("ERROR: can't issue service authorization (Illegal char in pexires in)");
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

	if (recorder_) { wrp_.version += " " + recorder_.version; }
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
