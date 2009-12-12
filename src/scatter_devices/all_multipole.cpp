/*
 *  scatterdevices.cpp
 *
 *  Created on: Dec 30, 2008
 *  Authors:
 *  Benjamin Lindner, ben@benlabs.net
 *
 *  Copyright 2008,2009 Benjamin Lindner
 *
 */

// direct header
#include "scatter_devices.hpp"

// standard header
#include <complex>
#include <fstream>

// special library headers
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/math/special_functions.hpp>

// other headers
#include "analysis.hpp"
#include "coor3d.hpp"
#include "decompose.hpp"
#include "control.hpp"
#include "sample.hpp"
#include "smath.hpp"
#include "particle_trajectory.hpp"
#include "vector_unfold.hpp"

using namespace std;

AllMSScatterDevice::AllMSScatterDevice(boost::mpi::communicator& thisworld, Sample& sample) {

	p_thisworldcomm = &thisworld;
	p_sample = &sample; // keep a reference to the sample

	string target = Params::Inst()->scattering.target;

	size_t nn = thisworld.size(); // Number of Nodes
	size_t na = sample.atoms.selections[target].size(); // Number of Atoms
	size_t nf = sample.coordinate_sets.size();

	size_t rank = thisworld.rank();
	EvenDecompose edecomp(nf,nn);
	
	myframes = edecomp.indexes_for(rank);
	if (thisworld.rank()==0) {	

        size_t memusage_scatmat = 2*sizeof(double)*myframes.size()*1;
                
        size_t memusage_per_cs = 3*sizeof(double)*na;
        size_t memusage_allcs = memusage_per_cs*myframes.size();
        
        
        Info::Inst()->write(string("Memory Requirements per node: "));
		Info::Inst()->write(string("Scattering Matrix: ")+to_s(memusage_scatmat)+string(" bytes"));


        // fault if not enough memory for scattering matrix
        if (memusage_scatmat>Params::Inst()->limits.memory.scattering_matrix) {
			Err::Inst()->write(string("Insufficient Buffer size for scattering matrix."));            
			Err::Inst()->write(string("Size required:")+to_s(memusage_scatmat)+string(" bytes"));            
			Err::Inst()->write(string("Configuration Parameter: limits.memory.scattering_matrix"));
            throw;
        }

		Info::Inst()->write(string("Coordinate Sets: ")+to_s(myframes.size()*memusage_per_cs)+string(" bytes"));		

        // warn if not enough memory for coordinate sets (cacheable system)
		if (Params::Inst()->runtime.limits.cache.coordinate_sets<myframes.size()) {
			Warn::Inst()->write(string("Insufficient Buffer size for coordinate sets. This is a HUGE bottleneck for performance!"));
			Warn::Inst()->write(string("Need at least: ")+to_s(memusage_allcs)+string(" bytes"));
			Warn::Inst()->write(string("Configuration Parameter: limits.memory.coordinate_sets"));
		}		
	}
    long lmax = Params::Inst()->scattering.average.orientation.multipole.resolution;
    
    long maxoffset = 0;
	for (int l=0;l<=lmax;l++) {
		for (int m=-l;m<=l;m++) { 
            maxoffset++;
		}
	}
	
	a.resize(myframes.size(),maxoffset); // n x 1 = frames x 1
	
	p_sample->coordinate_sets.set_representation(SPHERICAL);		
	p_sample->coordinate_sets.set_selection(p_sample->atoms.selections[target]);
	if (Params::Inst()->scattering.center) {
    	p_sample->coordinate_sets.add_postalignment(target,"center");		    
	}

	
	scatterfactors.set_sample(*p_sample);
	scatterfactors.set_selection(sample.atoms.selections[target]);
	scatterfactors.set_background(true);
	
}

// acts like scatter_frame, but does the summation in place
void AllMSScatterDevice::scatter_frame_norm1(size_t iframe, CartesianCoor3D& q) {

	size_t noa = p_sample->coordinate_sets.get_selection().size();
	
	// this is a specially hacked version of CoordinateSet , it contains r, phi, theta at x,y,z repectively
	timer.start("sd:fs:f:ld");	
	CoordinateSet& cs = p_sample->coordinate_sets.load(myframes[iframe]); 
	timer.stop("sd:fs:f:ld");	
	vector<double>& sfs = scatterfactors.get_all();

	using namespace boost::numeric::ublas::detail;
	
	long resolution = Params::Inst()->scattering.average.orientation.multipole.resolution;	
	const int lmax=int(resolution);
	
	std::vector<std::vector<complex<double> > > almv;

  	almv.resize(lmax+1);
  	for (int l=0;l<=lmax;l++) {
   		almv[l].resize(2*l+1,complex<double>(0,0));
  	}

	double ql = q.length();
	double M_PI_four = 4*M_PI;

	for(size_t j = 0; j < noa; ++j) {
		double r   = cs.c1[j];
	 	double phi = cs.c2[j];
		double theta = cs.c3[j];
				
		double esf = sfs[j];
		
		for (int l=0;l<=lmax;l++) {
			complex<double> fmpiilesf = M_PI_four*pow(complex<double>(0,1.0),l) * esf;
			double aabess = boost::math::sph_bessel(l,ql*r);
		
			for (int m=-l;m<=l;m++) {
		
			complex<double> aa = conj(boost::math::spherical_harmonic(l,m,theta,phi)); 

			almv[l][m+l] += fmpiilesf * aabess* aa;
			}
		}
	}

    long offset = 0;
	for (int l=0;l<=lmax;l++) {
		for (int m=-l;m<=l;m++) { 
            a(iframe,offset) = almv[l][m+l] / sqrt(M_PI_four);
            offset++;
		}
	}
}

void AllMSScatterDevice::scatter_frames_norm1(CartesianCoor3D& q) {
	
	for(size_t i = 0; i < myframes.size(); ++i)
	{
		timer.start("sd:fs:f");			    
		scatter_frame_norm1(i,q);
		timer.stop("sd:fs:f");				
	}
}

vector<complex<double> > AllMSScatterDevice::gather_frames() {
	// each node has computed their assigned frames
	// the total scattering amplitudes reside in a(x,0)

	//negotiate maximum size for coordinatesets
	size_t CSsize = myframes.size();
	size_t maxCSsize;
    timer.start("sd:gf:areduce");	
	boost::mpi::all_reduce(*p_thisworldcomm,CSsize,maxCSsize,boost::mpi::maximum<size_t>());
    timer.stop("sd:gf:areduce");

	// for multipole we already have the conj-multiplied version 

	vector<complex<double> > local_A;
	local_A.resize(maxCSsize,complex<double>(0,0));
	
	for(size_t ci = 0; ci < myframes.size(); ++ci)
	{
		local_A[ci] = a(ci,0);
	}

	vector<double> local_Ar = flatten(local_A);
	vector<double> all_Ar; all_Ar.resize(2*maxCSsize*p_thisworldcomm->size());	

    timer.start("sd:gf:gather");	
	boost::mpi::gather(*p_thisworldcomm,&local_Ar[0], 2*maxCSsize ,&all_Ar[0],0);
    timer.stop("sd:gf:gather");	

	if (p_thisworldcomm->rank()==0) {
		EvenDecompose edecomp(p_sample->coordinate_sets.size(),p_thisworldcomm->size());

		// this has interleaving A , have to be indexed away
		vector<complex<double> > A; A.resize(p_sample->coordinate_sets.size());	

		for(size_t i = 0; i < p_thisworldcomm->size(); ++i)
		{
			vector<size_t> findexes = edecomp.indexes_for(i);
			for(size_t j = 0; j < findexes.size(); ++j)
			{
				A[ findexes[j] ] = complex<double>(all_Ar[ 2*maxCSsize*i + 2*j ],all_Ar[ 2*maxCSsize*i + 2*j +1]);
			}
		}

		return A;
	} else {
		vector<complex<double> > A;
		return A;
	}
}


void AllMSScatterDevice::conjmultiply_frames() {
	for(size_t i = 0; i < a.size1(); ++i)
	{
        if (a.size2()<1) continue;
        a(i,0)=a(i,0)*conj(a(i,0));
	    for(size_t j = 1; j < a.size2(); ++j)
	    {
            a(i,0)+= a(i,j) * conj( a(i,j) );
	    }
    }
}


std::vector<std::complex<double> > AllMSScatterDevice::correlate_frames(long mpindex) {
	// each nodes has computed their assigned frames
	// the total scattering amplitudes reside in a(x,0)

    size_t NF = p_sample->coordinate_sets.size();
    size_t NN = p_thisworldcomm->size();
    
	//negotiate maximum size for coordinatesets
	size_t CSsize = myframes.size();
	size_t maxCSsize = CSsize;
	if (Params::Inst()->debug.barriers) p_thisworldcomm->barrier();
	timer.start("sd:corr::areduce");										
	boost::mpi::all_reduce(*p_thisworldcomm,CSsize,maxCSsize,boost::mpi::maximum<size_t>());
	timer.stop("sd:corr::areduce");										

	vector<complex<double> > local_A;
	local_A.resize(maxCSsize,complex<double>(0,0));
	
	for(size_t ci = 0; ci < CSsize; ++ci)
	{
		local_A[ci] = a(ci,mpindex);
	}

	vector<double> local_Ar = flatten(local_A);
	vector<double> all_Ar; all_Ar.resize(2*maxCSsize*NN);	

	if (Params::Inst()->debug.barriers) p_thisworldcomm->barrier();
	timer.start("sd:corr:agather");										
	boost::mpi::all_gather(*p_thisworldcomm,&local_Ar[0], 2*maxCSsize ,&all_Ar[0]);
	timer.stop("sd:corr:agather");										

	EvenDecompose edecomp(NF,NN);

	// this has interleaving A , have to be indexed away
	vector<complex<double> > A; A.resize(NF);	

	for(size_t i = 0; i < p_thisworldcomm->size(); ++i)
	{
		vector<size_t> findexes = edecomp.indexes_for(i);
		for(size_t j = 0; j < findexes.size(); ++j)
		{
			A[ findexes[j] ] = complex<double>(all_Ar[ 2*maxCSsize*i + 2*j ],all_Ar[ 2*maxCSsize*i + 2*j +1]);
		}
	}

	RModuloDecompose rmdecomp(NF,NN);
	vector<size_t> mysteps = rmdecomp.indexes_for(p_thisworldcomm->rank());
	
	vector<complex<double> > correlated_a; correlated_a.resize(NF,complex<double>(0,0));

	timer.start("sd:cor:correlate");
	
    complex<double> mean; mean =0.0;
	if (Params::Inst()->scattering.correlation.zeromean) {
	    for(size_t i = 0; i < NF; ++i)
	    {
            mean += A[i];
	    }
        mean = mean * (1.0/NF);
	}
												
	for(size_t i = 0; i < mysteps.size(); ++i)
	{
		size_t tau = mysteps[i];
		size_t last_starting_frame = NF-tau;
		for(size_t k = 0; k < last_starting_frame; ++k) // this iterates the starting frame
		{
			complex<double> a1 = A[k] - mean;
			complex<double> a2 = A[k+tau] - mean;
			correlated_a[tau] += conj(a1)*a2;
		}
		correlated_a[tau] /= (last_starting_frame); // maybe a conj. multiply here		
	}
	timer.stop("sd:corr:correlate");											

	vector<double> ain_r = flatten(correlated_a);
	
	vector<double> aout_r; aout_r.resize(ain_r.size());

	if (Params::Inst()->debug.barriers) p_thisworldcomm->barrier();
	timer.start("sd:corr:reduce");											
	boost::mpi::reduce(*p_thisworldcomm,&ain_r[0],ain_r.size(),&aout_r[0],std::plus<double>(),0);
	timer.stop("sd:corr:reduce");											

	return compress(aout_r);
}


void AllMSScatterDevice::superpose_spectrum(vector<complex<double> >& spectrum, vector<complex<double> >& fullspectrum) {
	for(size_t j = 0; j < spectrum.size(); ++j)
	{
		fullspectrum[j] += spectrum[j];
	}
}   

void AllMSScatterDevice::multiply_alignmentfactors(CartesianCoor3D q) {
    for(size_t i = 0; i < a.size1(); ++i)
	{
        if (a.size2()<1) continue;
        size_t iframe = myframes[i];
        vector<CartesianCoor3D> avectors = p_sample->coordinate_sets.get_postalignmentvectors(iframe);
        CartesianCoor3D& bigR = *(avectors.rbegin());
        complex<double> factor = exp(complex<double>(0,q*bigR));
	    for(size_t j = 0; j < a.size2(); ++j)
	    {
            a(i,j) = a(i,j) * factor;
	    }
    }
}



void AllMSScatterDevice::execute(CartesianCoor3D q) {
		
	/// k, qvectors are prepared:
	vector<complex<double> > spectrum; spectrum.resize(p_sample->coordinate_sets.size());

	timer.start("sd:sf:update");
	scatterfactors.update(q); // scatter factors only dependent on length of q, hence we can do it once before the loop
	timer.stop("sd:sf:update");
	
	timer.start("sd:fs");	    
	scatter_frames_norm1(q); // put summed scattering amplitudes into first atom entry
	timer.stop("sd:fs");
	
	if (Params::Inst()->scattering.center) {
        multiply_alignmentfactors(q);
	}
	
    	if (Params::Inst()->scattering.correlation.type=="time") {
    		if (Params::Inst()->scattering.correlation.method=="direct") {
    			timer.start("sd:correlate");
    			for(size_t i = 0; i < (1+4*(Params::Inst()->scattering.average.orientation.multipole.resolution)); ++i)
    			{
    			    vector<complex<double> > thisspectrum;
        			thisspectrum = correlate_frames(i); // if correlation, otherwise do a elementwise conj multiply here			
        			for(size_t csi = 0; csi < p_sample->coordinate_sets.size(); ++csi)
        			{
                        spectrum[csi]+=thisspectrum[csi];
        			}
    			}					
    			timer.stop("sd:correlate");					
    		} else if (Params::Inst()->scattering.correlation.method=="fftw") {
    //				timer.start("sd:correlate");
    //				thisspectrum = correlate_frames_fftw(); // if correlation, otherwise do a elementwise conj multiply here			
    //				timer.stop("sd:correlate");	
    			Err::Inst()->write("Correlation method not understood. Supported methods: direct ");
                throw;
    		} else {
    			Err::Inst()->write("Correlation method not understood. Supported methods: direct fftw");
    			throw;
    		}
    	} else {
    		timer.start("sd:conjmul");				
    		conjmultiply_frames();
            spectrum = gather_frames();
    		timer.stop("sd:conjmul");									
    	}
    			
//	if (Params::Inst()->scattering.correlation.type=="time") {
//		Err::Inst()->write("Correlation not supported with the multipole method for spherical averaging");
//		throw;
//	} else {
//		timer.start("sd:gatherframes");			    
//		spectrum = gather_frames();
//		timer.stop("sd:gatherframes");				
//	}
	
	m_spectrum = spectrum;
	
}

vector<complex<double> >& AllMSScatterDevice::get_spectrum() {
	return m_spectrum;
}



// cylindrical:::


AllMCScatterDevice::AllMCScatterDevice(boost::mpi::communicator& thisworld, Sample& sample) {

	p_thisworldcomm = &thisworld;
	p_sample = &sample; // keep a reference to the sample

	string target = Params::Inst()->scattering.target;

	size_t nn = thisworld.size(); // Number of Nodes
	size_t na = sample.atoms.selections[target].size(); // Number of Atoms
	size_t nf = sample.coordinate_sets.size();

	size_t rank = thisworld.rank();

	EvenDecompose edecomp(nf,nn);
	
	myframes = edecomp.indexes_for(rank);
	if (thisworld.rank()==0) {	

        size_t memusage_scatmat = 2*sizeof(double)*myframes.size()*1;
                
        size_t memusage_per_cs = 3*sizeof(double)*na;
        size_t memusage_allcs = memusage_per_cs*myframes.size();
        
        
        Info::Inst()->write(string("Memory Requirements per node: "));
		Info::Inst()->write(string("Scattering Matrix: ")+to_s(memusage_scatmat)+string(" bytes"));


        // fault if not enough memory for scattering matrix
        if (memusage_scatmat>Params::Inst()->limits.memory.scattering_matrix) {
			Err::Inst()->write(string("Insufficient Buffer size for scattering matrix."));            
			Err::Inst()->write(string("Size required:")+to_s(memusage_scatmat)+string(" bytes"));            
			Err::Inst()->write(string("Configuration Parameter: limits.memory.scattering_matrix"));
            throw;
        }

		Info::Inst()->write(string("Coordinate Sets: ")+to_s(myframes.size()*memusage_per_cs)+string(" bytes"));		

        // warn if not enough memory for coordinate sets (cacheable system)
		if (Params::Inst()->runtime.limits.cache.coordinate_sets<myframes.size()) {
			Warn::Inst()->write(string("Insufficient Buffer size for coordinate sets. This is a HUGE bottleneck for performance!"));
			Warn::Inst()->write(string("Need at least: ")+to_s(memusage_allcs)+string(" bytes"));
			Warn::Inst()->write(string("Configuration Parameter: limits.memory.coordinate_sets"));
		}		
	}
	a.resize(myframes.size(),1+4*Params::Inst()->scattering.average.orientation.multipole.resolution); // n x 1 = frames x 1
	
	p_sample->coordinate_sets.set_selection(sample.atoms.selections[target]);
	p_sample->coordinate_sets.set_representation(CYLINDRICAL);	
	if (Params::Inst()->scattering.center) {
    	p_sample->coordinate_sets.add_postalignment(target,"center");		    
	}
	
	scatterfactors.set_sample(sample);
	scatterfactors.set_selection(sample.atoms.selections[target]);
	scatterfactors.set_background(true);
	
}

// acts like scatter_frame, but does the summation in place
void AllMCScatterDevice::scatter_frame_norm1(size_t iframe, CartesianCoor3D& q) {

	size_t noa = p_sample->coordinate_sets.get_selection().size();
	
	CoordinateSet& cs = p_sample->coordinate_sets.load(myframes[iframe]); 
	vector<double>& sfs = scatterfactors.get_all();

	using namespace boost::numeric::ublas::detail;
	
	long resolution = Params::Inst()->scattering.average.orientation.multipole.resolution;	
	const int lmax=int(resolution);
	
	CartesianCoor3D o = Params::Inst()->scattering.average.orientation.multipole.axis;
	o = o / o.length();
	
	// get the part of the scattering vector perpenticular to the o- orientation
	CartesianCoor3D qparallel = (o*q)*o; 
	CartesianCoor3D qperpenticular = q - qparallel; // define this as phi=0 
	double qr = qperpenticular.length();
	double qz = qparallel.length();
    double ql = q.length();
	
	std::vector<complex<double> > A,B,C,D;
	A.resize(lmax+1,complex<double>(0,0));
	B.resize(lmax+1,complex<double>(0,0));
	C.resize(lmax+1,complex<double>(0,0));
	D.resize(lmax+1,complex<double>(0,0));


	for(size_t j = 0; j < noa; ++j) {
		double r   = cs.c1[j];
	 	double phi = cs.c2[j];
		double z = cs.c3[j];
				
		double esf = sfs[j];
		
		double psiphi = phi; // review this!
	
		double parallel_sign = 1.0;
		if ((z!=0) && (qz!=0)) {
			parallel_sign = (z*qz) / (abs(z)*abs(qz));			
		}

		complex<double> expi = exp(complex<double>(0,parallel_sign*z*qz));

		A[0] += expi * (double)boost::math::cyl_bessel_j(0,r*qr) * esf ;
	

		for (int l=1;l<=lmax;l++) {
			complex<double> fac1 = 2.0*powf(-1.0,l)*boost::math::cyl_bessel_j(2*l,r*qr);
			complex<double> fac2 = complex<double>(0,1.0)*double(2.0*powf(-1.0,l-1)*boost::math::cyl_bessel_j(2*l-1,r*qr));

			A[l] += fac1*expi*cos(2*l*psiphi) *esf;
			B[l] += fac1*expi*sin(2*l*psiphi) *esf;
			C[l] += fac2*expi*cos((2*l-1)*psiphi) *esf;
			D[l] += fac2*expi*sin((2*l-1)*psiphi) *esf;
		}
					
	}	
	
    a(iframe,0)=A[0];
	for (int l=1;l<=lmax;l++) {
        a(iframe, (l-1)*4 + 1 ) = sqrt(0.5)*A[l];
        a(iframe, (l-1)*4 + 2 ) = sqrt(0.5)*B[l];
        a(iframe, (l-1)*4 + 3 ) = sqrt(0.5)*C[l];
        a(iframe, (l-1)*4 + 4 ) = sqrt(0.5)*D[l];        
	}
	
//	complex<double> total(0,0);
//	total = A[0]* conj(A[0]); 		
//	for (int l=1;l<=lmax;l++) {
//		total += 0.5*A[l]*conj(A[l]);
//		total += 0.5*B[l]*conj(B[l]);
//		total += 0.5*C[l]*conj(C[l]);
//		total += 0.5*D[l]*conj(D[l]);
//	}
//	
//	a(iframe,0)=total; // sum at this location
}

std::vector<std::complex<double> > AllMCScatterDevice::correlate_frames(long mpindex) {
	// each nodes has computed their assigned frames
	// the total scattering amplitudes reside in a(x,0)

    size_t NF = p_sample->coordinate_sets.size();
    size_t NN = p_thisworldcomm->size();
    
	//negotiate maximum size for coordinatesets
	size_t CSsize = myframes.size();
	size_t maxCSsize = CSsize;
	if (Params::Inst()->debug.barriers) p_thisworldcomm->barrier();
	timer.start("sd:corr::areduce");										
	boost::mpi::all_reduce(*p_thisworldcomm,CSsize,maxCSsize,boost::mpi::maximum<size_t>());
	timer.stop("sd:corr::areduce");										

	vector<complex<double> > local_A;
	local_A.resize(maxCSsize,complex<double>(0,0));
	
	for(size_t ci = 0; ci < CSsize; ++ci)
	{
		local_A[ci] = a(ci,mpindex);
	}

	vector<double> local_Ar = flatten(local_A);
	vector<double> all_Ar; all_Ar.resize(2*maxCSsize*NN);	

	if (Params::Inst()->debug.barriers) p_thisworldcomm->barrier();
	timer.start("sd:corr:agather");										
	boost::mpi::all_gather(*p_thisworldcomm,&local_Ar[0], 2*maxCSsize ,&all_Ar[0]);
	timer.stop("sd:corr:agather");										

	EvenDecompose edecomp(NF,NN);

	// this has interleaving A , have to be indexed away
	vector<complex<double> > A; A.resize(NF);	

	for(size_t i = 0; i < p_thisworldcomm->size(); ++i)
	{
		vector<size_t> findexes = edecomp.indexes_for(i);
		for(size_t j = 0; j < findexes.size(); ++j)
		{
			A[ findexes[j] ] = complex<double>(all_Ar[ 2*maxCSsize*i + 2*j ],all_Ar[ 2*maxCSsize*i + 2*j +1]);
		}
	}

	RModuloDecompose rmdecomp(NF,NN);
	vector<size_t> mysteps = rmdecomp.indexes_for(p_thisworldcomm->rank());
	
	vector<complex<double> > correlated_a; correlated_a.resize(NF,complex<double>(0,0));

	timer.start("sd:cor:correlate");
	
    complex<double> mean; mean =0.0;
	if (Params::Inst()->scattering.correlation.zeromean) {
	    for(size_t i = 0; i < NF; ++i)
	    {
            mean += A[i];
	    }
        mean = mean * (1.0/NF);
	}
												
	for(size_t i = 0; i < mysteps.size(); ++i)
	{
		size_t tau = mysteps[i];
		size_t last_starting_frame = NF-tau;
		for(size_t k = 0; k < last_starting_frame; ++k) // this iterates the starting frame
		{
			complex<double> a1 = A[k] - mean;
			complex<double> a2 = A[k+tau] - mean;
			correlated_a[tau] += conj(a1)*a2;
		}
		correlated_a[tau] /= (last_starting_frame); // maybe a conj. multiply here		
	}
	timer.stop("sd:corr:correlate");											

	vector<double> ain_r = flatten(correlated_a);
	
	vector<double> aout_r; aout_r.resize(ain_r.size());

	if (Params::Inst()->debug.barriers) p_thisworldcomm->barrier();
	timer.start("sd:corr:reduce");											
	boost::mpi::reduce(*p_thisworldcomm,&ain_r[0],ain_r.size(),&aout_r[0],std::plus<double>(),0);
	timer.stop("sd:corr:reduce");											

	return compress(aout_r);
}


void AllMCScatterDevice::scatter_frames_norm1(CartesianCoor3D& q) {
	
	for(size_t i = 0; i < myframes.size(); ++i)
	{
		scatter_frame_norm1(i,q);
	}
}

void AllMCScatterDevice::conjmultiply_frames() {
	for(size_t i = 0; i < a.size1(); ++i)
	{
        if (a.size2()<1) continue;
        a(i,0)=a(i,0)*conj(a(i,0));
	    for(size_t j = 1; j < a.size2(); ++j)
	    {
            a(i,0)+= a(i,j) * conj( a(i,j) );
	    }
    }
}

vector<complex<double> > AllMCScatterDevice::gather_frames() {
	// each node has computed their assigned frames
	// the total scattering amplitudes reside in a(x,0)

	//negotiate maximum size for coordinatesets
	size_t CSsize = myframes.size();
	size_t maxCSsize;
	boost::mpi::all_reduce(*p_thisworldcomm,CSsize,maxCSsize,boost::mpi::maximum<size_t>());


	// for multipole we already have the conj-multiplied version 

	vector<complex<double> > local_A;
	local_A.resize(maxCSsize,complex<double>(0,0));
	
	for(size_t ci = 0; ci < myframes.size(); ++ci)
	{
		local_A[ci] = a(ci,0);
	}

	vector<double> local_Ar = flatten(local_A);
	vector<double> all_Ar; all_Ar.resize(2*maxCSsize*p_thisworldcomm->size());	

	boost::mpi::gather(*p_thisworldcomm,&local_Ar[0], 2*maxCSsize ,&all_Ar[0],0);

	if (p_thisworldcomm->rank()==0) {
		EvenDecompose edecomp(p_sample->coordinate_sets.size(),p_thisworldcomm->size());

		// this has interleaving A , have to be indexed away
		vector<complex<double> > A; A.resize(p_sample->coordinate_sets.size());	

		for(size_t i = 0; i < p_thisworldcomm->size(); ++i)
		{
			vector<size_t> findexes = edecomp.indexes_for(i);
			for(size_t j = 0; j < findexes.size(); ++j)
			{
				A[ findexes[j] ] = complex<double>(all_Ar[ 2*maxCSsize*i + 2*j ],all_Ar[ 2*maxCSsize*i + 2*j +1]);
			}
		}

		return A;
	} else {
		vector<complex<double> > A;
		return A;
	}
}

void AllMCScatterDevice::superpose_spectrum(vector<complex<double> >& spectrum, vector<complex<double> >& fullspectrum) {
	for(size_t j = 0; j < spectrum.size(); ++j)
	{
		fullspectrum[j] += spectrum[j];
	}
}

void AllMCScatterDevice::multiply_alignmentfactors(CartesianCoor3D q) {
    for(size_t i = 0; i < a.size1(); ++i)
	{
        if (a.size2()<1) continue;
        size_t iframe = myframes[i];
        vector<CartesianCoor3D> avectors = p_sample->coordinate_sets.get_postalignmentvectors(iframe);
        CartesianCoor3D& bigR = *(avectors.rbegin());
        complex<double> factor = exp(complex<double>(0,q*bigR));
	    for(size_t j = 0; j < a.size2(); ++j)
	    {
            a(i,j) = a(i,j) * factor;
	    }
    }
}


void AllMCScatterDevice::execute(CartesianCoor3D q) {
			

	/// k, qvectors are prepared:
	vector<complex<double> > spectrum; spectrum.resize(p_sample->coordinate_sets.size());

	timer.start("sd:sf:update");
	scatterfactors.update(q); // scatter factors only dependent on length of q, hence we can do it once before the loop
	timer.stop("sd:sf:update");
	
	timer.start("sd:fs");	    
	scatter_frames_norm1(q); // put summed scattering amplitudes into first atom entry
	timer.stop("sd:fs");

	if (Params::Inst()->scattering.center) {
        multiply_alignmentfactors(q);
	}

	if (Params::Inst()->scattering.correlation.type=="time") {
		if (Params::Inst()->scattering.correlation.method=="direct") {
			timer.start("sd:correlate");
			for(size_t i = 0; i < (1+4*(Params::Inst()->scattering.average.orientation.multipole.resolution)); ++i)
			{
			    vector<complex<double> > thisspectrum;
    			thisspectrum = correlate_frames(i); // if correlation, otherwise do a elementwise conj multiply here			
    			for(size_t csi = 0; csi < p_sample->coordinate_sets.size(); ++csi)
    			{
                    spectrum[csi]+=thisspectrum[csi];
    			}
			}					
			timer.stop("sd:correlate");					
		} else if (Params::Inst()->scattering.correlation.method=="fftw") {
//				timer.start("sd:correlate");
//				thisspectrum = correlate_frames_fftw(); // if correlation, otherwise do a elementwise conj multiply here			
//				timer.stop("sd:correlate");	
			Err::Inst()->write("Correlation method not understood. Supported methods: direct ");
            throw;
		} else {
			Err::Inst()->write("Correlation method not understood. Supported methods: direct fftw");
			throw;
		}
	} else {
		timer.start("sd:conjmul");				
		conjmultiply_frames();
        spectrum = gather_frames();
		timer.stop("sd:conjmul");									
	}

//	if (Params::Inst()->scattering.correlation.type=="time") {
//		Err::Inst()->write("Correlation not supported with the multipole method for cylindrical averaging");
//		throw;
//	} else {
//		spectrum = gather_frames();
//	}
	
	m_spectrum = spectrum;
}

vector<complex<double> >& AllMCScatterDevice::get_spectrum() {
	return m_spectrum;
}

