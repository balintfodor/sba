% Demo program for sba's MEX-file interface
% Performs Euclidean bundle adjustment with known camera intrinsics

% files containing initial motion & structure estimates and intrinsic parameters
camsfname='../demo/7cams.txt'; %'/home/lourakis/cmp/bt.cams';
ptsfname='../demo/7pts.txt'; %'/home/lourakis/cmp/bt.pts';
calfname='../demo/calib.txt'; %'/home/lourakis/cmp/bt.cal';

% read in camera parameters
[p1, p2, p3, p4, p5, p6, p7]=textread(camsfname, '%f%f%f%f%f%f%f', 'commentstyle', 'shell');
ncams=length(p1);
% normalize quaternions, ensure positiveness of scalar parts and keep vector parts
for i=1:ncams
  mag=sqrt(p1(i)^2 + p2(i)^2 + p3(i)^2 + p4(i)^2);
  if(p1(i)<0), mag=-mag; end;
  p1(i)=p1(i)/mag;
  p2(i)=p2(i)/mag;
  p3(i)=p3(i)/mag;
  p4(i)=p4(i)/mag;
end
r0=[p1, p2, p3, p4]; % save initial rotation
cams=[zeros(ncams, 1), zeros(ncams, 1), zeros(ncams, 1), p5, p6, p7]; % init local rotation to zero
cnp=size(cams, 2);
%cams

% read in point parameters
% use of a dense (dmask) and sparse (spmask) matrix for image projections is demonstrated.
% both matrices are created having one row and are augmented later
spmask=sparse([], [], [], 1, ncams);
dmask=zeros(1, ncams);
npts=0;
pts2D=[];
fid=fopen(ptsfname);
% read points file line by line
while ~feof(fid),
  line=fgets(fid);
  [A, count, errmsg, nextindex]=sscanf(line, '%f%f%f%f', [1, 4]); % read X, Y, Z, nframes
  if(size(A, 2)>0) % did we read anything?
    npts=npts+1;
    pts3D(npts, :)=A(1:3); % store X, Y, Z
    nframes=A(4);
    projs=zeros(1, 2*nframes); ptvis=zeros(1, ncams); % preallocate
    for i=1:nframes % read "nframes" id, x, y triplets
      [A, count, errmsg, j]=sscanf(line(nextindex:length(line)), '%f%f%f', [1, 3]); % read id, x, y
      nextindex=nextindex+j; % skip the already read line prefix 
      % store id, remember that matlab indices start from 1
      ptvis(A(1)+1)=1;
      spmask(npts, A(1)+1)=1;
      projs((i-1)*2+1:i*2)=A(2:3); % store x, y
    end
    % "projs" now has the image projections of point "npts" and "ptvis" is its visibility mask
    pts2D=[pts2D projs];
    dmask(npts, :)=ptvis;
  end
end
fclose(fid);
pnp=size(pts3D, 2);
%dmask
%pts2D
%pts3D

% read in calibration parameters
[a1, a2, a3]=textread(calfname, '%f%f%f', 'commentstyle', 'shell');
cal=[a1(1), a2(1), a3(1), a2(2), a3(2)];
%cal

% at this point, motion, structure and calibration parameters have been read and sba can be invoked

% initial camera & structure estimates vector
r0=reshape(r0', 1, numel(r0)); % convert to row vector, undo with reshape(r0, 4, ncams)'
p0=[reshape(cams', 1, ncams*cnp) reshape(pts3D', 1, npts*pnp)];
% options vector
opts=[1E-03, 1E-12, 1E-12, 1E-12, 0.0]; % setting to [] forces defaults to be used
% note how intrinsic calibration is passed as an extra argument to the invocations below

%profile clear
%profile on

% motion & structure BA
[ret, p, info]=sba(npts, 0, ncams, 1, spmask, p0, cnp, pnp, pts2D, 2, 'projRTS', 'jacprojRTS', 100, 1, opts, 'motstr', r0, cal);
% as above but without Jacobian
%[ret, p, info]=sba(npts, 0, ncams, 1, dmask, p0, cnp, pnp, pts2D, 2, 'projRTS', 100, 1, opts, r0, cal);


% motion & structure BA using C projection function & Jacobian from a shared library.
% Note: .so should be changed to .dll under windows
%[ret, p, info]=sba(npts, 0, ncams, 1, spmask, p0, cnp, pnp, pts2D, 2, 'imgproj_motstr@./projac.so', 'imgprojac_motstr@./projac.so', 100, 1, opts, 'motstr', r0, cal);


% motion only BA using C projection function & Jacobian from a shared library.
% Note: .so should be changed to .dll under windows
%[ret, p, info]=sba(npts, 0, ncams, 1, spmask, p0, cnp, pnp, pts2D, 2, 'imgproj_motstr@./projac.so', 'imgprojac_mot@./projac.so', 100, 1, opts, 'mot', r0, cal);

% structure only BA using C projection function & Jacobian from a shared library.
% Note: .so should be changed to .dll under windows
%[ret, p, info]=sba(npts, 0, ncams, 1, spmask, p0, cnp, pnp, pts2D, 2, 'imgproj_motstr@./projac.so', 'imgprojac_str@./projac.so', 100, 1, opts, 'str', r0, cal);


% motion only BA
%[ret, p, info]=sba(npts, 0, ncams, 1, spmask, p0, cnp, pnp, pts2D, 2, 'projRTS', 'jacprojRT', 100, 1, opts, 'mot', r0, cal);

% structure only BA
%[ret, p, info]=sba(npts, 0, ncams, 1, spmask, p0, cnp, pnp, pts2D, 2, 'projRTS', 'jacprojS', 100, 1, opts, 'str', r0, cal);

%profile off
%profile report


% sba finished; retrieve the refined motion & structure from the output vector
cams=reshape(p(1:ncams*cnp), cnp, ncams)';
% derive the full (local) unit quaternions from their vector parts and combine with the initial rotation
cams=[zeros(ncams, 1), cams]; % augment with a leading column
for i=1:ncams
  % note that the cams column indices below are incremented by one to account for the extra column above!
  qrl=[sqrt(1 - (cams(i, 2)^2 + cams(i, 3)^2 + cams(i, 4)^2)), cams(i, 2), cams(i, 3), cams(i, 4)];
  qr0=r0((i-1)*4+1:i*4);
  % tmp=qrl*qr0;
  tmp=[qrl(1)*qr0(1) - dot(qrl(2:4), qr0(2:4)),  qrl(1)*qr0(2:4) + qr0(1)*qrl(2:4) + cross(qrl(2:4), qr0(2:4))];
  if(tmp(1)<0), tmp=-tmp; end; % ensure scalar part is non-negative
  cams(i, 1:4)=tmp;
end
pts3D=reshape(p(ncams*cnp+1:ncams*cnp+npts*pnp), pnp, npts)';

% display results
%format long;
%cams
%pts3D
