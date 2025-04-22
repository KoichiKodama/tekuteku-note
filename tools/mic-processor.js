class MicProcessor extends AudioWorkletProcessor {
	static get parameterDescriptors() { return [{name:'chunk_size',defaultValue:16000}]; }

	constructor(options) {
		super();
		this.chunk_size = options.processorOptions.chunk_size;
		this.audioBuffer = [];
	}

	convertFloat32ToInt16(inputs) {
		const inputChannelData = inputs[0][0];
		const data = Int16Array.from(inputChannelData,(n)=>{
			const r = ( n < 0 ? n*32768 : n*32767 );
			return Math.max(-32768,Math.min(32767,r));
		});
		this.audioBuffer = Int16Array.from([...this.audioBuffer,...data]);
		if ( this.audioBuffer.length >= this.chunk_size ) {
			this.port.postMessage({eventType:"data",audioBuffer:this.audioBuffer,});
			this.audioBuffer = [];
		}
	}

	process(inputs) {
		if ( inputs[0].length === 0 ) { return false; }
		this.convertFloat32ToInt16(inputs);
		return true;
	}
}
registerProcessor("mic-processor",MicProcessor);
