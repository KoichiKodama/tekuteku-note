class mic_monitor_t {
	ui_element;
	context;
	analyzer;
	stream;
	source;
	data;
	timer;
	tooltip;
	label = '未確認';

	constructor(s) {
		this.ui_element = s;
		this.timer = 0;
	}
	async init() {
		this.context = new AudioContext();
		this.analyzer = this.context.createAnalyser();
		this.stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });
		this.source = this.context.createMediaStreamSource(this.stream);
		this.source.connect(this.analyzer);
		this.data = new Uint8Array(this.analyzer.fftSize);
		const device_id = this.stream.getAudioTracks()[0].getSettings().deviceId;
		const l = await navigator.mediaDevices.enumerateDevices();
		l.filter((device)=>( device.kind == 'audioinput' && device.deviceId == device_id )).forEach((device)=>{ this.label = device.label; });
	}
	start() {
		this.init().then(()=>{
			if ( this.context.state === 'suspended' ) { this.context.resume(); }
			this.timer = setInterval(()=>{
				this.analyzer.getByteTimeDomainData(this.data);
				const d = Array.from(this.data);
				const pk = Math.min(255,4*d.reduce((acc,c)=>Math.max(acc,Math.abs(c-128)),0));
				$(this.ui_element).css('background-color','rgb(0,'+pk+',0)');
			},100);
			$(this.ui_element).attr({'data-bs-toggle':'tooltip','data-bs-placement':'top','title':this.label});
			this.tooltip = new bootstrap.Tooltip($(this.ui_element)[0],{trigger:'hover'});
		});
	}
	stop() {
		if ( this.timer > 0 ) {
			clearInterval(this.timer);
			this.timer = 0;
			this.source.disconnect();
			this.stream.getTracks().forEach( track => track.stop() );
			this.tooltip.dispose();
			$(this.ui_element).removeAttr('data-bs-toggle data-bs-placement title');
		}
	}
};

class mic_stream_t {
	config = {
		channles: 1,
		sample_rate: 16000,
		sample_size: 16,
		chunk_size: 16000 
	};

	debug = true;
	socket;
	stream;
	context;
	source;
	worklet;

	constructor(socket) { this.socket = socket; }

	async open() {
		let mic_spec = {
			video:false, 
			audio:{
				channelCount: this.config.channels,
				sampleRate: this.config.sample_rate,
				sampleSize: this.config.sample_size,
				voiceIsolation: true 
			} 
		};
		this.stream = await navigator.mediaDevices.getUserMedia(mic_spec)
		this.context = new AudioContext({sampleRate: this.config.sample_rate});
		this.context.suspend();
		if ( Math.round(this.context.sampleRate) != this.config.sample_rate ) alert(`mic_stream_t::open() : context.sampleRate=${this.context.sampleRate}`);
		this.source = this.context.createMediaStreamSource(this.stream);
		await this.context.audioWorklet.addModule("./tools/mic-processor.js");
		this.worklet = new AudioWorkletNode(this.context,"mic-processor",{processorOptions:{chunk_size: this.config.chunk_size},});
		this.worklet.port.onmessage = (e)=>{
			if ( e.data.eventType === "data" ) {
				this.socket.send(e.data.audioBuffer);
			}
		};
		this.source.connect(this.worklet);
		this.worklet.connect(this.context.destination);
	};

	start() {
		this.open().then(()=>{
			if ( this.context.state === 'suspended' ) this.context.resume();
		});
	};

	stop() {
		if ( this.context.state === 'running' ) this.context.suspend();
		this.stream.getTracks().forEach((track)=>track.stop());
		this.source.disconnect();
		this.worklet.disconnect();
	};
};

//
// 以下は記録として残しておく
//

function save_wav(fname_wav,chunks) {
	const len = chunks.reduce((acc,cur)=>acc+cur.byteLength,0);
	const h = mk_wav_header(bytes_per_sample,channels,sample_rate,len);
	const data = [h,...chunks];
	const b = new Blob(data,{type:"audio/wav"});
	const url = URL.createObjectURL(b);
	const e = document.createElement("a");
	document.body.appendChild(e);
	e.download = fname_wav;
	e.href = url;
	e.click();
	e.remove();
	URL.revokeObjectURL(url);
}

function write_string(dataView,offset,string) { for (let i=0;i<string.length;i++) { dataView.setUint8(offset+i,string.charCodeAt(i)); } }

function mk_wav_header(bytes_per_sample,channels,sample_rate,data_length) {
	const header = new ArrayBuffer(44);
	const view = new DataView(header);
	write_string(view,0,"RIFF"); // RIFF identifier 'RIFF'
	view.setUint32(4,36+data_length,true); // file length minus RIFF identifier length and file description length
	write_string(view,8,"WAVE"); // RIFF type 'WAVE'
	write_string(view,12,"fmt "); // format chunk identifier 'fmt '
	view.setUint32(16,16,true); // format chunk length
	view.setUint16(20,1,true); // sample format (raw)
	view.setUint16(22,1,true); // channel count
	view.setUint32(24,sample_rate,true); // sample rate
	view.setUint32(28,sample_rate*bytes_per_sample*channels,true); // byte rate (sample rate * block align)
	view.setUint16(32,bytes_per_sample*channels,true); // block align (channel count * bytes per sample)
	view.setUint16(34,8*bytes_per_sample,true); // bits per sample
	write_string(view, 36,"data"); // data chunk identifier 'data'
	view.setUint32(40,data_length,true); // data chunk length
	return header;
}
