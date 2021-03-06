/* Project: Laser odometry
   Author: Mariano Jaimez Tarifa
   Date: January 2015 */


#include <mrpt/nav/reactive/CReactiveNavigationSystem3D.h>
#include <mrpt/opengl.h>
#include <mrpt/opengl/CPlanarLaserScan.h>
#include <mrpt/maps/COccupancyGridMap2D.h>
#include <mrpt/utils/CRobotSimulator.h>
#include <mrpt/gui.h>
#include <mrpt/utils/round.h>
#include <mrpt/system/filesystem.h>
#include <mrpt/math/lightweight_geom_data.h>
#include "laser_odometry_v1.h"
#include "laser_odometry_3scans.h"
#include "polar_match.h"
#include "csm/csm_all.h"
//#include "csm/sm/csm/csm_all.h"


using namespace mrpt;
using namespace mrpt::utils;
using namespace mrpt::nav;
using namespace mrpt::opengl;
using namespace mrpt::maps;
using namespace mrpt::obs;
using namespace mrpt::gui;
using namespace mrpt::poses;
using namespace mrpt::math;
using namespace CSM;
using namespace std;



struct TRobotLaser {
	CObservation2DRangeScan m_scan;
    CObservation2DRangeScan m_scan_old;
	int						m_level;
	int						m_segments;
};


class CMyReactInterface : public CReactiveInterfaceImplementation
{
public:

	CPose2D					new_pose;
	CPose2D					last_pose;
	CPose2D					target;
	CRobotSimulator			robotSim;
	TRobotShape				robotShape;
	COccupancyGridMap2D		map;
	TRobotLaser				laser;
	CDisplayWindow3D		window;
	COpenGLScenePtr			scene;

    //RF2O
    RF2O      odo;
    RF2O      odo_test;

    //Polar scan matcher
    CPose2D     new_psm_pose, old_psm_pose;
    PMScan      ls, ls_ref;
    bool        matchFailed;

    //Canonical scan matcher
    sm_params params;               //Structure containing the input parameters
    sm_result result;               //Output of the scan matching
    LDP laser_ref, laser_sens;      //the laser scans to be matched (in the PL-ICP data structure)
    int recover_from_error;
    CPose2D new_csm_pose, old_csm_pose;

    //Results
    vector<CPose3D>	real_poses, est_poses, test_poses, psm_poses, csm_poses;
    float est_time, test_time, psm_time, csm_time;

    //Experiments
    float x_target[7], y_target[7];

	
	bool getCurrentPoseAndSpeeds( poses::CPose2D &curPose, float &curV, float &curW)
	{
		robotSim.getRealPose( curPose );
		curV = robotSim.getV();
		curW = robotSim.getW();
		return true;
	}

	bool changeSpeeds( float v, float w )
	{
		robotSim.movementCommand(v,w);
		return true;
	}

	bool senseObstacles( CSimplePointsMap 	&obstacles )
	{
		last_pose = new_pose;
        laser.m_scan_old = laser.m_scan;
		robotSim.getRealPose(new_pose);
		CPose3D robotpose3d(new_pose[0], new_pose[1], 0, new_pose[2], 0, 0);

		obstacles.clear();

		//Laser scans
		map.laserScanSimulator(laser.m_scan, new_pose, 0.5f, laser.m_segments, laser.m_scan.stdError, 1, 0);
		
		//Correct points with null observations (set them to 0)
		for (unsigned int i=0; i<laser.m_scan.scan.size(); i++)
            if (laser.m_scan.scan[i] > 0.995f*laser.m_scan.maxRange)
            {
				laser.m_scan.scan[i] = 0.f;
                laser.m_scan.validRange[i] = false;
            }
            else
            {
                laser.m_scan.validRange[i] = true;
            }
		obstacles.insertObservation(&laser.m_scan);

		return true;
	}

	void loadMaps( const utils::CConfigFileBase &ini )
	{
		CImage myImg;
        float resolution = 0.016; //0.02f, 0.04f
        //myImg.loadFromXPM(map_xpm); //map_xpm, map_lab_xpm
        //map.loadFromBitmap(myImg,resolution);

        map.loadFromBitmapFile("../map_simulator.bmp", resolution);

		std::cout << std::endl << "The map have been loaded successfully.";
	}

	void loadConfiguration( const utils::CConfigFileBase &ini )
	{
		unsigned int num_levels;
		std::vector <float> lasercoord, xaux, yaux;

		//Read laser params
		ini.read_vector("LASER_CONFIG","LASER_POSE", std::vector<float> (0), lasercoord , true);
		laser.m_scan.maxRange = ini.read_float("LASER_CONFIG","LASER_MAX_RANGE", 50, true);
		laser.m_scan.aperture = DEG2RAD(ini.read_float("LASER_CONFIG","LASER_APERTURE", 180, true));
		laser.m_scan.stdError = ini.read_float("LASER_CONFIG","LASER_STD_ERROR", 0.05, true);
		laser.m_scan.sensorPose.setFromValues(lasercoord[0],lasercoord[1],lasercoord[2],lasercoord[3],lasercoord[4],lasercoord[5]);
		laser.m_level = ini.read_int("LASER_CONFIG","LASER_LEVEL", 1, true);
		laser.m_segments = ini.read_int("LASER_CONFIG","LASER_SEGMENTS", 181, true);

		//Read config params which describe the robot shape
		num_levels = ini.read_int("ROBOT_CONFIG","HEIGHT_LEVELS", 1, true);
		robotShape.polygons.resize(num_levels);
		robotShape.heights.resize(num_levels);
		for (unsigned int i=1;i<=num_levels;i++)
		{
			robotShape.heights[i-1] = ini.read_float("ROBOT_CONFIG",format("LEVEL%d_HEIGHT",i), 1, true);
			ini.read_vector("ROBOT_CONFIG",format("LEVEL%d_VECTORX",i), std::vector<float> (0), xaux, false);
			ini.read_vector("ROBOT_CONFIG",format("LEVEL%d_VECTORY",i), std::vector<float> (0), yaux, false);
			ASSERT_(xaux.size() == yaux.size());
			for (unsigned int j=0;j<xaux.size();j++)
			{
				robotShape.polygons[i-1].AddVertex(xaux[j], yaux[j]);
			}
		}

		//Read other params associated with the robot model and its navigation
		float tau = ini.read_float("NAVIGATION_CONFIG","ROBOTMODEL_TAU", 0, true);
		float delay = ini.read_float("NAVIGATION_CONFIG","ROBOTMODEL_DELAY", 0, true);
		float x_ini = ini.read_float("NAVIGATION_CONFIG","X0", 0, true);
		float y_ini = ini.read_float("NAVIGATION_CONFIG","Y0", 0, true);
		float phi_ini = DEG2RAD(ini.read_float("NAVIGATION_CONFIG","PHI0", 0, true));
		robotSim.setDelayModelParams(tau, delay);
		robotSim.resetStatus();
		robotSim.setOdometryErrors(0);
		robotSim.setRealPose(CPose2D(x_ini, y_ini, phi_ini));
	}

    void initializeEverything()
    {
        odo.initialize(laser.m_segments, laser.m_scan.aperture, false);
        odo_test.initialize(laser.m_segments, laser.m_scan.aperture, true);
        initializeScene();

        pm_init();          //Contains precomputed range bearings and their sines/cosines
        Init_Params_csm();
        loadFirstScanRF2O();
        setRF2OPose(new_pose);
        new_psm_pose = new_pose; old_psm_pose = new_pose;
        new_csm_pose = new_pose; old_csm_pose = new_pose;
        est_time = 0.f; test_time = 0.f; psm_time = 0.f; csm_time = 0.f;
    }

	void initializeScene()
	{
		CPose3D robotpose3d;
		robotpose3d.x(robotSim.getX());
		robotpose3d.y(robotSim.getY());
		robotpose3d.setYawPitchRoll(robotSim.getPHI(),0,0);

		//The display window is created
		mrpt::global_settings::OCTREE_RENDER_MAX_POINTS_PER_NODE = 10000;
		window.setWindowTitle("Reactive Navigation. Robot motion simulation");
        window.resize(1200,980);
        window.setPos(800,0);
		window.setCameraZoom(35);
		window.setCameraAzimuthDeg(20);
		window.setCameraElevationDeg(60);
		scene = window.get3DSceneAndLock();

		//Map is inserted
		CSetOfObjectsPtr gl_grid = CSetOfObjects::Create();
		map.getAs3DObject(gl_grid);
		scene->insert(gl_grid);

		//A CornerXYZ object is inserted as an absolute frame of reference
		CSetOfObjectsPtr ref = opengl::stock_objects::CornerXYZ();
		ref->setLocation(0,0,0);
		scene->insert( ref );

		//The target is inserted
		CDiskPtr target = opengl::CDisk::Create(0.4, 0.3);
		target->setLocation(0, 0, 0);
		target->setColor(0.2,0.3,0.9);
		scene->insert( target );

		//Robots
		CPolyhedronPtr robot_real;
		robot_real = opengl::CPolyhedron::CreateCustomPrism(robotShape.polygons[0], robotShape.heights[0]);
		robot_real->setName("robot_real");
		robot_real->setPose(robotpose3d);
		robot_real->setColor(0.9,0.1,0);
		robot_real->setWireframe(true);
		robot_real->setLineWidth(2);
		scene->insert( robot_real );

		CPolyhedronPtr robot_est;
		robot_est = opengl::CPolyhedron::CreateCustomPrism(robotShape.polygons[0], robotShape.heights[0]);
		robot_est->setName("robot_est");
		robot_est->setPose(robotpose3d);
		robot_est->setColor(0.2,0.5,0.2);
		robot_est->setWireframe(true);
		robot_est->setLineWidth(2);
		scene->insert( robot_est );

		CPolyhedronPtr robot_test;
		robot_test = opengl::CPolyhedron::CreateCustomPrism(robotShape.polygons[0], robotShape.heights[0]);
		robot_test->setName("robot_test");
		robot_test->setPose(robotpose3d);
		robot_test->setColor(0.2,0.2,0.9);
		robot_test->setWireframe(true);
		robot_test->setLineWidth(2);
		scene->insert( robot_test );

        CPolyhedronPtr robot_psm;
        robot_psm = opengl::CPolyhedron::CreateCustomPrism(robotShape.polygons[0], robotShape.heights[0]);
        robot_psm->setName("robot_psm");
        robot_psm->setPose(robotpose3d);
        robot_psm->setColor(0.f,0.f,0.f);
        robot_psm->setWireframe(true);
        robot_psm->setLineWidth(2);
        scene->insert( robot_psm );

        CPolyhedronPtr robot_csm;
        robot_csm = opengl::CPolyhedron::CreateCustomPrism(robotShape.polygons[0], robotShape.heights[0]);
        robot_csm->setName("robot_csm");
        robot_csm->setPose(robotpose3d);
        robot_csm->setColor(0.9f,0.5f,0.f);
        robot_csm->setWireframe(true);
        robot_csm->setLineWidth(2);
        scene->insert( robot_csm );


        //Initialization of the scans. One scan is simulated
        //-------------------------------------------------------------------------
		CSimplePointsMap auxpoints;
		senseObstacles( auxpoints );
        laser.m_scan_old = laser.m_scan;

        //psm
        pm_readScan(laser.m_scan, &ls);
        pm_preprocessScan(&ls);
        matchFailed = false;

        //csm
        laser_ref = cast_CObservation2DRangeScan_to_LDP(laser.m_scan);
        // For the first scan, set estimate = odometry
        copy_d(laser_ref->odometry, 3, laser_ref->estimate);
        //--------------------------------------------------------------------------

		//The laserscan is inserted
		CPointCloudColouredPtr gl_laser = CPointCloudColoured::Create();
		gl_laser->setPointSize(5.f);
		gl_laser->setPose(robotpose3d);
		scene->insert( gl_laser );

		//Trajectories
		CSetOfLinesPtr traj_lines_real = opengl::CSetOfLines::Create();
		traj_lines_real->setColor(1,0,0);
		traj_lines_real->setLineWidth(4);
		scene->insert( traj_lines_real );
		opengl::CSetOfLinesPtr traj_lines_est = opengl::CSetOfLines::Create();
		traj_lines_est->setColor(0,1,0);
		traj_lines_est->setLineWidth(4);
		scene->insert( traj_lines_est );
		opengl::CSetOfLinesPtr traj_lines_test = opengl::CSetOfLines::Create();
		traj_lines_test->setColor(0,0,1);
		traj_lines_test->setLineWidth(4);
		scene->insert( traj_lines_test );
        opengl::CSetOfLinesPtr traj_lines_psm = opengl::CSetOfLines::Create();
        traj_lines_psm->setColor(0,0,0);
        traj_lines_psm->setLineWidth(4);
        scene->insert( traj_lines_psm );
        opengl::CSetOfLinesPtr traj_lines_csm = opengl::CSetOfLines::Create();
        traj_lines_csm->setColor(0.9f,0.5f,0.0);
        traj_lines_csm->setLineWidth(4);
        scene->insert( traj_lines_csm );

		//Normals
		//CVectorField3DPtr normals = CVectorField3D::Create();
		//normals->setVectorFieldColor(0.3,0.3,1);
		//normals->setLineWidth(2.f);
		//scene->insert( normals );

		window.unlockAccess3DScene();
		std::string legend;
		legend.append("--------------------------------------------\n");
		legend.append("| n - Simulate one step \n");
		legend.append("| s - Start/stop continuous sim. \n");
		legend.append("| m - Move the target \n");
		legend.append("| c - Reset estimated pose \n");
		legend.append("| e - Exit \n");
		legend.append("--------------------------------------------\n");
		legend.append(format("\n        %.02fFPS", window.getRenderingFPS()));
		
		window.addTextMessage(5,180, legend,utils::TColorf(1,1,1),"Arial",13);
		window.repaint();
	}

	void updateScene()
	{
		scene = window.get3DSceneAndLock();
		CPose3D robotpose3d;
		CRenderizablePtr obj;
		robotpose3d.x(robotSim.getX());
		robotpose3d.y(robotSim.getY());
		robotpose3d.setYawPitchRoll(robotSim.getPHI(),0,0);

		//Robots
		obj = scene->getByName("robot_real");
		obj->setPose(robotpose3d);

		obj = scene->getByName("robot_est");
		obj->setPose(odo.laser_pose);

		obj = scene->getByName("robot_test");
		obj->setPose(odo_test.laser_pose);

        obj = scene->getByName("robot_psm");
        obj->setPose(new_psm_pose);

        obj = scene->getByName("robot_csm");
        obj->setPose(new_csm_pose);

		const unsigned int repr_level = round(log2(round(float(odo.width)/float(odo.cols))));

		//Laser
		CPose3D laserpose;
		laser.m_scan.getSensorPose(laserpose);
		CPointCloudColouredPtr gl_laser;
		gl_laser = scene->getByClass<CPointCloudColoured> (0);
		gl_laser->clear();
		for (unsigned int i=0; i<odo.cols; i++)
			gl_laser->push_back(odo.xx[repr_level](i), odo.yy[repr_level](i), 0.1, 1-sqrt(odo.weights(i)), sqrt(odo.weights(i)), 0);

		gl_laser->setPose(robotpose3d + laserpose);

		//Trajectories
		opengl::CSetOfLinesPtr traj_lines_real;
		traj_lines_real = scene->getByClass<CSetOfLines> (0);
		traj_lines_real->appendLine(last_pose[0], last_pose[1], 0.2, new_pose[0], new_pose[1], 0.2);

		opengl::CSetOfLinesPtr traj_lines_est;
		traj_lines_est = scene->getByClass<CSetOfLines> (1);
		traj_lines_est->appendLine(odo.laser_oldpose[0], odo.laser_oldpose[1], 0.2, odo.laser_pose[0], odo.laser_pose[1], 0.2);

		opengl::CSetOfLinesPtr traj_lines_test;
		traj_lines_test = scene->getByClass<CSetOfLines> (2);
		traj_lines_test->appendLine(odo_test.laser_oldpose[0], odo_test.laser_oldpose[1], 0.2, odo_test.laser_pose[0], odo_test.laser_pose[1], 0.2);

        opengl::CSetOfLinesPtr traj_lines_psm;
        traj_lines_psm = scene->getByClass<CSetOfLines> (3);
        traj_lines_psm->appendLine(old_psm_pose[0], old_psm_pose[1], 0.2, new_psm_pose[0], new_psm_pose[1], 0.2);

        opengl::CSetOfLinesPtr traj_lines_csm;
        traj_lines_csm = scene->getByClass<CSetOfLines> (4);
        traj_lines_csm->appendLine(old_csm_pose[0], old_csm_pose[1], 0.2, new_csm_pose[0], new_csm_pose[1], 0.2);

		//Normals
		//MatrixXf aux = 0.f*odo.xx[repr_level];
		//CVectorField3DPtr normals = scene->getByClass<CVectorField3D> (0);
		//normals->setPose(robotpose3d + laserpose);
		//normals->setPointCoordinates(odo.xx[repr_level], odo.yy[repr_level], aux);
		//MatrixXf nz(1,odo.cols);
		//nz.assign(0.f);
		//normals->setVectorField(odo.normx, odo.normy, nz);


		window.unlockAccess3DScene();
		std::string legend;
		legend.append("--------------------------------------------\n");
		legend.append("| n - Simulate one step \n");
		legend.append("| s - Start/stop continuous sim. \n");
		legend.append("| m - Move the target \n");
		legend.append("| c - Reset estimated pose \n");
		legend.append("| e - Exit \n");
		legend.append("--------------------------------------------\n");
		legend.append(format("\n        %.02fFPS", window.getRenderingFPS()));
		
		window.addTextMessage(5,180, legend,utils::TColorf(1,1,1),"Arial",13);
		window.repaint();
	}

	void resetScene()
	{
		scene = window.get3DSceneAndLock();
		CPose3D robotpose3d;
		CRenderizablePtr obj;
		robotpose3d.x(robotSim.getX());
		robotpose3d.y(robotSim.getY());
		robotpose3d.setYawPitchRoll(robotSim.getPHI(),0,0);

		odo.laser_pose = CPose2D(robotpose3d);
		odo.laser_oldpose = CPose2D(robotpose3d);
        odo.kai_abs.assign(0.f);
        odo.kai_loc.assign(0.f);
        odo.kai_loc_old.assign(0.f);

		odo_test.laser_pose = CPose2D(robotpose3d);
		odo_test.laser_oldpose = CPose2D(robotpose3d);
        odo_test.kai_abs.assign(0.f);
        odo_test.kai_loc.assign(0.f);
        odo_test.kai_loc_old.assign(0.f);

        new_psm_pose = CPose2D(robotpose3d);
        old_psm_pose = CPose2D(robotpose3d);

        new_csm_pose = CPose2D(robotpose3d);
        old_csm_pose = CPose2D(robotpose3d);

		//Robots
		obj = scene->getByName("robot_real");
		obj->setPose(robotpose3d);

		obj = scene->getByName("robot_est");
		obj->setPose(odo.laser_pose);

		obj = scene->getByName("robot_test");
		obj->setPose(odo_test.laser_pose);

        obj = scene->getByName("robot_psm");
        obj->setPose(new_psm_pose);

        obj = scene->getByName("robot_csm");
        obj->setPose(new_csm_pose);


		//Laser
		const unsigned int repr_level = round(log2(round(float(odo.width)/float(odo.cols))));

		CPose3D laserpose;
		laser.m_scan.getSensorPose(laserpose);
		CPointCloudColouredPtr gl_laser;
		gl_laser = scene->getByClass<CPointCloudColoured> (0);
		gl_laser->clear();
		for (unsigned int i=0; i<odo.cols; i++)
			gl_laser->push_back(odo.xx[repr_level](i), odo.yy[repr_level](i), 0.1, 1-sqrt(odo.weights(i)), sqrt(odo.weights(i)), 0);

		gl_laser->setPose(robotpose3d + laserpose);

		//Trajectories
		opengl::CSetOfLinesPtr traj_lines_real;
		traj_lines_real = scene->getByClass<CSetOfLines> (0);
		traj_lines_real->clear();

		opengl::CSetOfLinesPtr traj_lines_est;
		traj_lines_est = scene->getByClass<CSetOfLines> (1);
		traj_lines_est->clear();

		opengl::CSetOfLinesPtr traj_lines_test;
		traj_lines_test = scene->getByClass<CSetOfLines> (2);
		traj_lines_test->clear();

        opengl::CSetOfLinesPtr traj_lines_psm;
        traj_lines_psm = scene->getByClass<CSetOfLines> (3);
        traj_lines_psm->clear();

        opengl::CSetOfLinesPtr traj_lines_csm;
        traj_lines_csm = scene->getByClass<CSetOfLines> (4);
        traj_lines_csm->clear();


		window.unlockAccess3DScene();
		std::string legend;
		legend.append("--------------------------------------------\n");
		legend.append("| n - Simulate one step \n");
		legend.append("| s - Start/stop continuous sim. \n");
		legend.append("| m - Move the target \n");
		legend.append("| c - Reset estimated pose \n");
		legend.append("| e - Exit \n");
		legend.append("--------------------------------------------\n");
		legend.append(format("\n        %.02fFPS", window.getRenderingFPS()));
		
		window.addTextMessage(5,180, legend,utils::TColorf(1,1,1),"Arial",13);
		window.repaint();
	}


	CAbstractReactiveNavigationSystem::TNavigationParams createNewTarget(float x,
					float y, float targetAllowedDistance, bool targetIsRelative = 0)
	{
		CPose2D robotpose;
		CAbstractReactiveNavigationSystem::TNavigationParams navparams;
		navparams.target = mrpt::math::TPoint2D(x,y);
		navparams.targetAllowedDistance = targetAllowedDistance;
		navparams.targetIsRelative = targetIsRelative;
		if (targetIsRelative == 0)
			target = CPose2D(x, y, 0);
		else
		{
			robotSim.getRealPose(robotpose);
			target = CPose2D(x ,y, 0) + robotpose;
		}
		return navparams;
	}

    void runRF2O()
    {
        //Run the linear version
        for (unsigned int i=0; i<odo.width; i++)
            odo.range_wf(i) = laser.m_scan.scan[i];

        odo.odometryCalculation();
        est_time += odo.runtime;

        //Run the robust nonlinear version
        for (unsigned int i=0; i<odo_test.width; i++)
            odo_test.range_wf(i) = laser.m_scan.scan[i];

        odo_test.odometryCalculation();
        test_time += odo_test.runtime;
    }

    void loadFirstScanRF2O()
    {
        //Linear version
        for (unsigned int i=0; i<odo.width; i++)
            odo.range_wf(i) = laser.m_scan.scan[i];
        odo.createScanPyramid();

        //Nonlinear version
        for (unsigned int i=0; i<odo_test.width; i++)
            odo_test.range_wf(i) = laser.m_scan.scan[i];
        odo_test.createScanPyramid();
    }

    void setRF2OPose(const CPose2D &reset_pose)
    {
        odo.laser_pose = reset_pose;
        odo.laser_oldpose = reset_pose;

        odo_test.laser_pose = reset_pose;
        odo_test.laser_oldpose = reset_pose;
    }

	void computeErrors(unsigned int react_freq)
	{
		const unsigned int size_v = real_poses.size();
        CPose3D incr_real, incr_est, incr_test, incr_psm, incr_csm;
        CPose3D dif_est, dif_test, dif_psm, dif_csm;
        float aver_trans_error_est = 0.f, aver_rot_error_est = 0.f;
        float aver_trans_error_test = 0.f, aver_rot_error_test = 0.f;
        float aver_trans_error_psm = 0.f, aver_rot_error_psm = 0.f;
        float aver_trans_error_csm = 0.f, aver_rot_error_csm = 0.f;
        float aver_v = 0.f, aver_w = 0.f;
        float dist = 0.f;
		for (unsigned int i=0; i<size_v-react_freq; i++)
		{
			//Compute increments per second
			incr_real = real_poses.at(i+react_freq) - real_poses.at(i);
			incr_est = est_poses.at(i+react_freq) - est_poses.at(i);
			incr_test = test_poses.at(i+react_freq) - test_poses.at(i);
            incr_psm = psm_poses.at(i+react_freq) - psm_poses.at(i);
            incr_csm = csm_poses.at(i+react_freq) - csm_poses.at(i);

			//Compute differences between the estimates and the real one
			dif_est = incr_real - incr_est;
			dif_test = incr_real - incr_test;
            dif_psm = incr_real - incr_psm;
            dif_csm = incr_real - incr_csm;

            //Add their contribution to the average errors
            aver_trans_error_est += square(dif_est[0]) + square(dif_est[1]);
            aver_trans_error_test += square(dif_test[0]) + square(dif_test[1]);
            aver_trans_error_psm += square(dif_psm[0]) + square(dif_psm[1]);
            aver_trans_error_csm += square(dif_csm[0]) + square(dif_csm[1]);
            aver_rot_error_est += square(57.3f*dif_est.yaw());
            aver_rot_error_test += square(57.3f*dif_test.yaw());
            aver_rot_error_psm += square(57.3f*dif_psm.yaw());
            aver_rot_error_csm += square(57.3f*dif_csm.yaw());
        }


        aver_trans_error_est = sqrtf(aver_trans_error_est/(size_v-react_freq));
        aver_trans_error_test = sqrtf(aver_trans_error_test/(size_v-react_freq));
        aver_trans_error_psm = sqrtf(aver_trans_error_psm/(size_v-react_freq));
        aver_trans_error_csm = sqrtf(aver_trans_error_csm/(size_v-react_freq));
        aver_rot_error_est = sqrtf(aver_rot_error_est/(size_v-react_freq));
        aver_rot_error_test = sqrtf(aver_rot_error_test/(size_v-react_freq));
        aver_rot_error_psm = sqrtf(aver_rot_error_psm/(size_v-react_freq));
        aver_rot_error_csm = sqrtf(aver_rot_error_csm/(size_v-react_freq));

        for (unsigned int i=0; i<size_v-1; i++)
        {
            float incr_dist = sqrtf(square(real_poses.at(i+1)[0] - real_poses.at(i)[0]) + square(real_poses.at(i+1)[1] - real_poses.at(i)[1]));
            float incr_phi = abs(real_poses.at(i+1)[3] - real_poses.at(i)[3]);
            aver_v += react_freq*incr_dist;
            aver_w += 57.3f*react_freq*incr_phi;
            dist += incr_dist;
        }

        //Show the errors
        printf("\n Number of measurements = %d", size_v);
        printf("\n Aver_trans_est = %f m, Aver_rot_est = %f grad", aver_trans_error_est, aver_rot_error_est);
        printf("\n Aver_trans_test = %f m, Aver_rot_test = %f grad", aver_trans_error_test, aver_rot_error_test);
        printf("\n Aver_trans_psm = %f m, Aver_rot_psm = %f grad", aver_trans_error_psm, aver_rot_error_psm);
        printf("\n Aver_trans_csm = %f m, Aver_rot_csm = %f grad", aver_trans_error_csm, aver_rot_error_csm);
        printf("\n Runtimes: rf2o_lin = %f, rf2o_rob = %f, psm = %f, csm = %f", est_time/size_v, test_time/size_v, psm_time/size_v, csm_time/size_v);
        printf("\n Aver v = %f, aver w = %f, distance travelled = %f", aver_v/size_v, aver_w/size_v, dist);
        fflush(stdout);
	}

    void computeErrorsPerLength()
    {
        const unsigned int size_v = real_poses.size();
        CPose3D incr_real, incr_est, incr_test, incr_psm, incr_csm;
        CPose3D dif_est, dif_test, dif_psm, dif_csm;
        float aver_trans_error_est, aver_rot_error_est;
        float aver_trans_error_test, aver_rot_error_test;
        float aver_trans_error_psm, aver_rot_error_psm;
        float aver_trans_error_csm, aver_rot_error_csm;
        float dist[3] = {-0.02f, -0.02f, -0.02f};
        unsigned int ini_i[3] = {0, 216, 400}; //freq = 5 Hz
        unsigned int end_i[3] = {216, 400, size_v-1}; //freq = 5 Hz
        ofstream file_res;

        //Open file
        file_res.open("./results_all_scenario.txt");


        for (unsigned int s=0; s<3; s++)
        {

            //Compute distance travelled in this scenario
            for (unsigned int i=ini_i[s]; i<end_i[s]; i++)
                dist[s] += sqrtf(square(real_poses.at(i+1)[0] - real_poses.at(i)[0]) + square(real_poses.at(i+1)[1] - real_poses.at(i)[1]));

            //Evaluate the drift per segment length for a number of possible lengths
            const unsigned int num_segments = 50;

            for (unsigned int k = 1; k<=num_segments; k++)
            {
                const float length = k*dist[s]/num_segments;
                unsigned int i = ini_i[s];
                unsigned int num_samp = 0;

                //Initialize error metrics
                aver_trans_error_est = 0.f; aver_rot_error_est = 0.f; aver_trans_error_test = 0.f; aver_rot_error_test = 0.f;
                aver_trans_error_psm = 0.f; aver_rot_error_psm = 0.f; aver_trans_error_csm = 0.f; aver_rot_error_csm = 0.f;

                while (true)
                {
                    //1. Find the pose after travelling a "length" distance
                    unsigned int l = i+1;

                    if (l > end_i[s])    break;

                    float local_dist = sqrtf(square(real_poses.at(l)[0] - real_poses.at(i)[0]) + square(real_poses.at(l)[1] - real_poses.at(i)[1]));
                    while (local_dist < length)
                    {
                        l++;
                        if (l > end_i[s])    break;
                        local_dist += sqrtf(square(real_poses.at(l)[0] - real_poses.at(l-1)[0]) + square(real_poses.at(l)[1] - real_poses.at(l-1)[1]));
                    }

                    if (l > end_i[s])    break;
                    num_samp++;

                    incr_real = real_poses.at(l) - real_poses.at(i);
                    incr_est = est_poses.at(l) - est_poses.at(i);
                    incr_test = test_poses.at(l) - test_poses.at(i);
                    incr_psm = psm_poses.at(l) - psm_poses.at(i);
                    incr_csm = csm_poses.at(l) - csm_poses.at(i);

                    //Compute differences between the estimates and the real one
                    dif_est = incr_real - incr_est;
                    dif_test = incr_real - incr_test;
                    dif_psm = incr_real - incr_psm;
                    dif_csm = incr_real - incr_csm;

                    //Add their contribution to the average errors
                    aver_trans_error_est += square(dif_est[0]) + square(dif_est[1]);
                    aver_trans_error_test += square(dif_test[0]) + square(dif_test[1]);
                    aver_trans_error_psm += square(dif_psm[0]) + square(dif_psm[1]);
                    aver_trans_error_csm += square(dif_csm[0]) + square(dif_csm[1]);
                    aver_rot_error_est += square(57.3f*dif_est.yaw());
                    aver_rot_error_test += square(57.3f*dif_test.yaw());
                    aver_rot_error_psm += square(57.3f*dif_psm.yaw());
                    aver_rot_error_csm += square(57.3f*dif_csm.yaw());

                    i++;
                }

                aver_trans_error_est = sqrtf(aver_trans_error_est/num_samp);
                aver_trans_error_test = sqrtf(aver_trans_error_test/num_samp);
                aver_trans_error_psm = sqrtf(aver_trans_error_psm/num_samp);
                aver_trans_error_csm = sqrtf(aver_trans_error_csm/num_samp);
                aver_rot_error_est = sqrtf(aver_rot_error_est/num_samp);
                aver_rot_error_test = sqrtf(aver_rot_error_test/num_samp);
                aver_rot_error_psm = sqrtf(aver_rot_error_psm/num_samp);
                aver_rot_error_csm = sqrtf(aver_rot_error_csm/num_samp);


                //Show the errors
                printf("\n Length of this stretch = %f, num_samp = %d, scenario %d", length, num_samp, s);
                printf("\n Aver_trans_est = %f m, Aver_rot_est = %f grad", aver_trans_error_est, aver_rot_error_est);
                printf("\n Aver_trans_test = %f m, Aver_rot_test = %f grad", aver_trans_error_test, aver_rot_error_test);
                printf("\n Aver_trans_psm = %f m, Aver_rot_psm = %f grad", aver_trans_error_psm, aver_rot_error_psm);
                printf("\n Aver_trans_csm = %f m, Aver_rot_csm = %f grad", aver_trans_error_csm, aver_rot_error_csm);
                fflush(stdout);

                //Save results in file
                file_res << s << " " << length << " ";
                file_res << aver_trans_error_test << " " << aver_trans_error_psm << " " << aver_trans_error_csm << endl;
            }
        }

        //Close file
        file_res.close();
        printf("\n Results saved");
    }



    //One of the compared methods (PSM)
    void runPolarScanMatching()
    {
        //Read scans (ls and ls_ref)
        //pm_readScan(laser.m_scan_old, &ls_ref);
        ls_ref = ls;
        pm_readScan(laser.m_scan, &ls);

        CTicTac clock; clock.Tic();

        //preprocess the new scan
        pm_preprocessScan(&ls);
        //pm_preprocessScan(&ls_ref);


        //Initial seed
        if (matchFailed)
        {
            ls.rx = 0.f; ls_ref.rx = 0.f;
            ls.ry = 0.f; ls_ref.ry = 0.f;
            ls.th = 0.f; ls_ref.th = 0.f;
        }
        else
        {
            ls.rx = 100.f*new_psm_pose[1]; ls_ref.rx = 100.f*old_psm_pose[1];
            ls.ry = 100.f*new_psm_pose[0]; ls_ref.ry = 100.f*old_psm_pose[0];
            ls.th = new_psm_pose[2]; ls_ref.th = old_psm_pose[2];
        }

        //printf("\n Initial motion: (%f, %f, %f), (%f, %f, %f)", 0.01f*ls.rx, 0.01f*ls.ry, ls.th, 0.01f*ls_ref.rx, 0.01f*ls_ref.ry, ls_ref.th);

        matchFailed = false;
        try
        {
          pm_psm(&ls_ref,&ls);
        }catch(int err)
        {
          cerr << "Error caught: failed match." << endl;
          matchFailed = true;
        }

        PM_TYPE err_idx = pm_error_index2(&ls_ref,&ls);
        //cout <<" err: "<<err_idx;

        if (abs(ls.ry) > 80.f) ls.ry = 0.f;
        if (abs(ls.rx) > 80.f) ls.rx = 0.f;
        if (abs(ls.th) > DEG2RAD(70.f)) ls.th = 0.f;

        //printf("\n Motion (x, y, phi) = (%f, %f, %f)", 0.01f*ls.rx, 0.01f*ls.ry, ls.th);

        const float psm_t = 1000.f*clock.Tac();
        psm_time += psm_t;

        printf("\nPSM runtime = %f ms", psm_t);
        fflush(stdout);

        old_psm_pose = new_psm_pose;
        new_psm_pose = old_psm_pose + CPose2D(0.01f*ls.ry,0.01f*ls.rx,ls.th);
    }

    void Init_Params_csm()
    {
        params.max_angular_correction_deg = 60;             //"Maximum angular displacement between scans"
        params.max_linear_correction = 1.0;                 //"Maximum translation between scans (m)"
        params.max_iterations = 100;                       //"When we had enough" 1000

        params.epsilon_xy = 0.0001;                         //"A threshold for stopping (m)" 0.0001
        params.epsilon_theta = 0.0001;                      //"A threshold for stopping (rad)" 0.0001

        params.max_correspondence_dist = 2.0;
        params.sigma = 0.01;                                //Noise in the scan

        params.use_corr_tricks = 1;                         //"Use smart tricks for finding correspondences."
        params.restart = 0;                                 //"Restart: Restart if error is over threshold"
        params.restart_threshold_mean_error = 0.01;
        params.restart_dt= 0.01;
        params.restart_dtheta = 1.5 * 3.14 /180;

        params.clustering_threshold = 0.05;                 //"Max distance for staying in the same clustering"
        params.orientation_neighbourhood = 3;               //"Number of neighbour rays used to estimate the orientation."

        params.use_point_to_line_distance = 1;              //"If 0, it's vanilla ICP."
        params.do_alpha_test = 0;                           //"Discard correspondences based on the angles"
        params.do_alpha_test_thresholdDeg = 20.0;           //

        params.outliers_maxPerc = 0.95;
        params.outliers_adaptive_order = 0.7;
        params.outliers_adaptive_mult = 2.0;
        params.do_visibility_test = 0;

        params.outliers_remove_doubles = 1;                 //"no two points in laser_sens can have the same corr."
        params.do_compute_covariance = 0;                   //"If 1, computes the covariance of ICP using the method http://purl.org/censi/2006/icpcov ."
        params.debug_verify_tricks = 0;                     //"Checks that find_correspondences_tricks gives the right answer.

        params.laser[0] = 0.0;                              //Pose of sensor with respect to robot: used for computing the first estimate given the odometry.
        params.laser[1] = 0.0;
        params.laser[2] = 0.0;

        params.min_reading = 0.0;                           //Don't use readings less than min_reading (m)
        params.max_reading = 5.6;                        //Don't use readings longer than max_reading (m)

        params.use_ml_weights = 0;                          //"If 1, the field 'true_alpha' (or 'alpha') in the first scan is used to compute the incidence beta, and the factor (1/cos^2(beta)) used to weight the correspondence.");
        params.use_sigma_weights = 0;                       //"If 1, the field 'readings_sigma' in the second scan is used to weight the correspondence by 1/sigma^2"
        recover_from_error = 0;
        printf("[Init_Params]: Params structure sucessfully initialized.\n\n");
    }

    LDP cast_CObservation2DRangeScan_to_LDP(CObservation2DRangeScan &laser_obj)
    {
        int n;
        n = laser_obj.scan.size();		//Num of samples (rays) of the scan laser

        LDP ld = ld_alloc_new(n);       //create new structure initialized with "n" rays

        ld->min_theta = -laser_obj.aperture/2;			//rad
        ld->max_theta = laser_obj.aperture/2;			//rad
        float angle_ray = laser_obj.aperture/(n-1);    //rad

        double index_cero_angle = floor( (n-1)/2);

        //scan readings
        for (int i=0; i<n; i++)
        {
            if (laser_obj.validRange[i] == false)
            {
                ld->readings[i] = NAN;                  //*(ld->readings+i)
                ld->valid[i] = 0;
            }
            else
            {
                ld->readings[i] = static_cast<double>(laser_obj.scan[i]);   //m
                ld->valid[i] = 1;                       //int
            }

            ld->theta[i] = (i-index_cero_angle)*angle_ray;             //rad
            ld->readings_sigma[i] = laser_obj.stdError;
            ld->cluster[i] = -1;                        //No cluster selected
        }

        //To avoid failure due to precission, copy min and max theta (see ld_valid_fields)
        ld->theta[0] = ld->min_theta;
        ld->theta[n-1] = ld->max_theta;

        //Set initial guess (odometry) to 0 (mandatory for proper funtionality)
        ld->odometry[0] = 0.0;
        ld->odometry[1] = 0.0;
        ld->odometry[2] = 0.0;

    /*
        mrpt::poses::CPose3D LaserPoseOnTheRobot;
        laser_obj.getSensorPose(LaserPoseOnTheRobot);
        //Set the initial pose
        laser_pose = LaserPoseOnTheRobot;
        laser_oldpose = LaserPoseOnTheRobot;

        jo_read_double_array(jo, "odometry", ld->odometry, 3, NAN);
        jo_read_double_array(jo, "estimate", ld->estimate, 3, NAN);
        jo_read_double_array(jo, "true_pose", ld->true_pose, 3, NAN);
    */

        double time_seconds = mrpt::system::timestampToDouble(laser_obj.timestamp);
        double fractpart, intpart;
        fractpart = modf(time_seconds, &intpart);
        ld->tv.tv_sec = intpart;
        ld->tv.tv_usec = fractpart;

        if(!ld_valid_fields(ld))
        {
            sm_error("[Init] Invalid laser data in first scan.\n");
            return ld;
        }

        //Structure is ready!
        return ld;
    }

    /* Runs the PL-ICP */
    bool runCanonicalScanMatching()
    {
        laser_sens = cast_CObservation2DRangeScan_to_LDP(laser.m_scan);
        CTicTac clock; clock.Tic();

        try
        {
            //set the scan data into the input structure
            params.laser_ref  = laser_ref;
            params.laser_sens = laser_sens;

            // Set first guess as the difference in odometry
            if (any_nan(params.laser_ref->odometry,3) || any_nan(params.laser_sens->odometry,3) )
            {
                sm_error("The 'odometry' field is set to NaN so I don't know how to get an initial guess. I usually use the difference in the odometry fields to obtain the initial guess.\n");
                sm_error("  laser_ref->odometry = %s \n",  friendly_pose(params.laser_ref->odometry) );
                sm_error("  laser_sens->odometry = %s \n", friendly_pose(params.laser_sens->odometry) );
                sm_error(" I will quit it here. \n");
                return false;
            }

            double odometry[3];
            pose_diff_d(laser_sens->odometry, laser_ref->odometry, odometry);
            double ominus_laser[3], temp[3];
            ominus_d(params.laser, ominus_laser);
            oplus_d(ominus_laser, odometry, temp);
            oplus_d(temp, params.laser, params.first_guess);

            // Do the actual work
            sm_icp(&params, &result);

        }
        catch(exception &e)
        {
            cerr << "[Matching] Returning false due to exception: " << endl;
            cerr << e.what() << endl;
            return false;
        }
        catch(...)
        {
            cerr << "[Matching] Returning false due to unknown exception: " << endl;
            return false;
        }

        if(!result.valid)
        {
            if(recover_from_error)
            {
                sm_info("One ICP matching failed. Because you passed  -recover_from_error, I will try to recover."
                " Note, however, that this might not be good in some cases. \n");
                sm_info("The recover is that the displacement is set to 0. No result stats is output. \n");

                /* For the first scan, set estimate = odometry */
                copy_d(laser_ref->estimate, 3, laser_sens->estimate);
                ld_free(laser_ref); laser_ref = laser_sens;
            }
            else
            {
                sm_error("One ICP matching failed. Because I process recursively, I will stop here.\n");
                sm_error("Use the option -recover_from_error if you want to try to recover.\n");
                ld_free(laser_ref);
                return 2;
            }
        }
        else
        {
            /* Add the result to the previous estimate */
            oplus_d(laser_ref->estimate, result.x, laser_sens->estimate);

            old_csm_pose = new_csm_pose;
            new_csm_pose = old_csm_pose + CPose2D(result.x[0],result.x[1],result.x[2]);

            //Free reference scan, and update with the sensed one.
            ld_free(laser_ref); laser_ref = laser_sens;
        }

        const float csm_t = 1000.f*clock.Tac();
        csm_time += csm_t;
        printf("\nCSM runtime = %f ms \n", csm_t);
    }

    void saveScans()
    {
        ofstream	m_fres;

        m_fres.open("./scan_first.txt");

        for (unsigned int i=0; i<laser.m_segments; i++)
            m_fres << i << " " << laser.m_scan.scan[i] << "\n";

        m_fres.close();

        m_fres.open("./scan_second.txt");

        for (unsigned int i=0; i<laser.m_segments; i++)
            m_fres << i << " " << laser.m_scan_old.scan[i] << "\n";

        m_fres.close();

        printf("\n Scans saved");

        CPose2D psm_sol = new_psm_pose - old_psm_pose;
        CPose2D real_sol = new_pose - last_pose;
        cout << "\n estimated motion = " << psm_sol;
        cout << "\n real motion = " << real_sol;
        fflush(stdout);
    }
    
    void saveResults(unsigned int freq)
    {
        ofstream	m_fres;
        char        aux[100];
        int         nFile = 0;
        bool        free_name = false;
        string      name;

        while (!free_name)
        {
            nFile++;
            sprintf(aux, "results_%03u.txt", nFile );
            name = aux;
            free_name = !system::fileExists(name);
        }
        m_fres.open(aux);


        //Save freq, real_pose (x,y,theta), est_pose (x,y,theta)
        for (unsigned int i=0; i<real_poses.size(); i++)
        {
            m_fres << freq << " ";
            m_fres << real_poses[i][0] << " " << real_poses[i][1] << " " << real_poses[i][3] << " ";
            m_fres << est_poses[i][0] << " " << est_poses[i][1] << " " << est_poses[i][3] << " ";
            m_fres << "\n";
        }

        m_fres.close();

        printf("\n Results saved in file \n");
    }

};


//Without different parts, a general one
//void computeErrorsPerLength()
//{
//    const unsigned int size_v = real_poses.size();
//    CPose3D incr_real, incr_est, incr_test, incr_psm, incr_csm;
//    CPose3D dif_est, dif_test, dif_psm, dif_csm;
//    float aver_trans_error_est, aver_rot_error_est;
//    float aver_trans_error_test, aver_rot_error_test;
//    float aver_trans_error_psm, aver_rot_error_psm;
//    float aver_trans_error_csm, aver_rot_error_csm;
//    float dist = 0.f;

//    //Compute distance travelled
//    for (unsigned int i=0; i<size_v-1; i++)
//        dist += sqrtf(square(real_poses.at(i+1)[0] - real_poses.at(i)[0]) + square(real_poses.at(i+1)[1] - real_poses.at(i)[1]));

//    //Evaluate the drift per segment length for a number of possible lengths
//    const unsigned int num_segments = 10;

//    for (unsigned int k = 1; k<=num_segments; k++)
//    {
//        const float length = k*dist/num_segments;
//        unsigned int i = 0;
//        unsigned int num_samp = 0;

//        //Initialize error metrics
//        aver_trans_error_est = 0.f; aver_rot_error_est = 0.f; aver_trans_error_test = 0.f; aver_rot_error_test = 0.f;
//        aver_trans_error_psm = 0.f; aver_rot_error_psm = 0.f; aver_trans_error_csm = 0.f; aver_rot_error_csm = 0.f;

//        while (true)
//        {
//            //1. Find the pose after travelling a "lenght" distance
//            unsigned int l = i+1;

//            if (l >= size_v)    break;

//            float local_dist = sqrtf(square(real_poses.at(l)[0] - real_poses.at(i)[0]) + square(real_poses.at(l)[1] - real_poses.at(i)[1]));
//            while (local_dist < length)
//            {
//                l++;
//                if (l >= size_v)    break;
//                local_dist += sqrtf(square(real_poses.at(l)[0] - real_poses.at(l-1)[0]) + square(real_poses.at(l)[1] - real_poses.at(l-1)[1]));
//            }

//            if (l >= size_v)    break;
//            num_samp++;

//            incr_real = real_poses.at(l) - real_poses.at(i);
//            incr_est = est_poses.at(l) - est_poses.at(i);
//            incr_test = test_poses.at(l) - test_poses.at(i);
//            incr_psm = psm_poses.at(l) - psm_poses.at(i);
//            incr_csm = csm_poses.at(l) - csm_poses.at(i);

//            //Compute differences between the estimates and the real one
//            dif_est = incr_real - incr_est;
//            dif_test = incr_real - incr_test;
//            dif_psm = incr_real - incr_psm;
//            dif_csm = incr_real - incr_csm;

//            //Add their contribution to the average errors
//            aver_trans_error_est += square(dif_est[0]) + square(dif_est[1]);
//            aver_trans_error_test += square(dif_test[0]) + square(dif_test[1]);
//            aver_trans_error_psm += square(dif_psm[0]) + square(dif_psm[1]);
//            aver_trans_error_csm += square(dif_csm[0]) + square(dif_csm[1]);
//            aver_rot_error_est += square(57.3f*dif_est.yaw());
//            aver_rot_error_test += square(57.3f*dif_test.yaw());
//            aver_rot_error_psm += square(57.3f*dif_psm.yaw());
//            aver_rot_error_csm += square(57.3f*dif_csm.yaw());

//            i++;
//        }

//        aver_trans_error_est = sqrtf(aver_trans_error_est/num_samp);
//        aver_trans_error_test = sqrtf(aver_trans_error_test/num_samp);
//        aver_trans_error_psm = sqrtf(aver_trans_error_psm/num_samp);
//        aver_trans_error_csm = sqrtf(aver_trans_error_csm/num_samp);
//        aver_rot_error_est = sqrtf(aver_rot_error_est/num_samp);
//        aver_rot_error_test = sqrtf(aver_rot_error_test/num_samp);
//        aver_rot_error_psm = sqrtf(aver_rot_error_psm/num_samp);
//        aver_rot_error_csm = sqrtf(aver_rot_error_csm/num_samp);


//        //Show the errors
//        printf("\n Length of this stretch = %f, num_samp = %d", length, num_samp);
//        printf("\n Aver_trans_est = %f m, Aver_rot_est = %f grad", aver_trans_error_est, aver_rot_error_est);
//        printf("\n Aver_trans_test = %f m, Aver_rot_test = %f grad", aver_trans_error_test, aver_rot_error_test);
//        printf("\n Aver_trans_psm = %f m, Aver_rot_psm = %f grad", aver_trans_error_psm, aver_rot_error_psm);
//        printf("\n Aver_trans_csm = %f m, Aver_rot_csm = %f grad", aver_trans_error_csm, aver_rot_error_csm);
//        fflush(stdout);
//    }
//}



